#include "StereoDuplexEncoderNode.h"

#include "3rdparty/edlib/edlib/include/edlib.h"
#include "utils/duplex_utils.h"
#include "utils/sequence_utils.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

using namespace torch::indexing;

namespace dorado {
std::shared_ptr<dorado::Read> StereoDuplexEncoderNode::stereo_encode(
        std::shared_ptr<dorado::Read> template_read,
        std::shared_ptr<dorado::Read> complement_read) {
    // We rely on the incoming read raw data being of type float16 to allow direct memcpy
    // of tensor elements.
    assert(template_read->raw_data.dtype() == torch::kFloat16);
    assert(complement_read->raw_data.dtype() == torch::kFloat16);
    using SampleType = c10::Half;

    std::shared_ptr<dorado::Read> read = std::make_shared<dorado::Read>();  // Return read

    // We align the reverse complement of the complement read to the template read.
    const auto complement_sequence_reverse_complement =
            dorado::utils::reverse_complement(complement_read->seq);

    // Align the two reads to one another and print out the score.
    EdlibAlignConfig align_config = edlibDefaultAlignConfig();
    align_config.task = EDLIB_TASK_PATH;

    EdlibAlignResult result =
            edlibAlign(template_read->seq.data(), template_read->seq.size(),
                       complement_sequence_reverse_complement.data(),
                       complement_sequence_reverse_complement.size(), align_config);

    int query_cursor = 0;
    int target_cursor = result.startLocations[0];
    float alignment_error_rate = (float)result.editDistance / (float)result.alignmentLength;

    auto [alignment_start_end, cursors] = dorado::utils::get_trimmed_alignment(
            11, result.alignment, result.alignmentLength, target_cursor, query_cursor, 0,
            result.endLocations[0]);

    query_cursor = cursors.first;
    target_cursor = cursors.second;
    int start_alignment_position = alignment_start_end.first;
    int end_alignment_position = alignment_start_end.second;

    // TODO: its overkill having this function make this decision...
    const int kMinTrimmedAlignmentLength = 50;
    const bool consensus_possible =
            (start_alignment_position < end_alignment_position) &&
            ((end_alignment_position - start_alignment_position) > kMinTrimmedAlignmentLength) &&
            alignment_error_rate < 0.2;

    if (!consensus_possible) {
        // There wasn't a good enough match -- return early with an empty read.
        edlibFreeAlignResult(result);
        ++m_num_discarded_pairs;
        return read;
    }

    // Edlib doesn't provide named constants for alignment array entries, so do it here.
    static constexpr unsigned char kAlignMatch = 0;
    static constexpr unsigned char kAlignInsertionToTarget = 1;
    static constexpr unsigned char kAlignInsertionToQuery = 2;
    static constexpr unsigned char kAlignMismatch = 3;

    // Move along the alignment, filling out the stereo-encoded tensor
    const int max_size = template_read->raw_data.size(0) + complement_read->raw_data.size(0);
    const auto opts = torch::TensorOptions().dtype(torch::kFloat16).device(torch::kCPU);

    static constexpr int kNumFeatures = 13;
    // Indices of features in the first dimension of the output tensor.
    static constexpr int kFeatureTemplateSignal = 0;
    static constexpr int kFeatureComplementSignal = 1;
    static constexpr int kFeatureTemplateFirstNucleotide = 2;
    static constexpr int kFeatureComplementFirstNucleotide = 6;
    static constexpr int kFeatureMoveTable = 10;
    static constexpr int kFeatureTemplateQScore = 11;
    static constexpr int kFeatureComplementQScore = 12;
    auto tmp = torch::zeros({kNumFeatures, max_size}, opts);

    int template_signal_cursor = 0;
    int complement_signal_cursor = 0;

    std::vector<uint8_t> template_moves_expanded;
    for (int i = 0; i < template_read->moves.size(); i++) {
        template_moves_expanded.push_back(template_read->moves[i]);
        for (int j = 0; j < m_input_signal_stride - 1; j++) {
            template_moves_expanded.push_back(0);
        }
    }

    int extra_padding = template_read->raw_data.size(0) - template_moves_expanded.size();
    for (int i = 0; i < extra_padding; i++) {
        template_moves_expanded.push_back(0);
    }

    int template_moves_seen = template_moves_expanded[template_signal_cursor];
    while (template_moves_seen < target_cursor + 1) {
        template_signal_cursor++;
        template_moves_seen += template_moves_expanded[template_signal_cursor];
    }

    std::vector<uint8_t> complement_moves_expanded;
    for (int i = 0; i < complement_read->moves.size(); i++) {
        complement_moves_expanded.push_back(complement_read->moves[i]);
        for (int j = 0; j < m_input_signal_stride - 1; j++) {
            complement_moves_expanded.push_back(0);
        }
    }

    extra_padding = complement_read->raw_data.size(0) - complement_moves_expanded.size();
    for (int i = 0; i < extra_padding; i++) {
        complement_moves_expanded.push_back(0);
    }
    complement_moves_expanded.push_back(1);
    std::reverse(complement_moves_expanded.begin(), complement_moves_expanded.end());
    complement_moves_expanded.pop_back();

    auto complement_signal = complement_read->raw_data;
    complement_signal = torch::flip(complement_read->raw_data, 0);

    int complement_moves_seen = complement_read->moves[complement_signal_cursor];
    while (complement_moves_seen < query_cursor + 1) {
        complement_signal_cursor++;
        complement_moves_seen += complement_moves_expanded[complement_signal_cursor];
    }

    const float pad_value = 0.8 * std::min(torch::min(complement_signal).item<float>(),
                                           torch::min(template_read->raw_data).item<float>());

    // Start with all signal feature entries equal to the padding value.
    tmp.index({torch::indexing::Slice(None, 2)}) = pad_value;

    // libtorch indexing calls go on a carefree romp through various heap
    // allocations/deallocations and object constructions/destructions, and so are
    // glacially slow.  We therefore work with raw pointers within the main loop.
    const auto* const template_raw_data_ptr =
            static_cast<SampleType*>(template_read->raw_data.data_ptr());
    const auto* const flipped_complement_raw_data_ptr =
            static_cast<SampleType*>(complement_signal.data_ptr());

    std::array<SampleType*, kNumFeatures> feature_ptrs;
    for (int feature_idx = 0; feature_idx < kNumFeatures; ++feature_idx) {
        auto& feature_ptr = feature_ptrs[feature_idx];
        feature_ptr = static_cast<SampleType*>(tmp[feature_idx].data_ptr());
    }

    int stereo_global_cursor = 0;  // Index into the stereo-encoded signal
    for (int i = start_alignment_position; i < end_alignment_position; i++) {
        // We move along every alignment position. For every position we need to add signal and padding.
        // Let us add the respective nucleotides and q-scores
        int template_segment_length = 0;    // index into this segment in signal-space
        int complement_segment_length = 0;  // index into this segment in signal-space

        // If there is *not* an insertion to the query, add the nucleotide from the target cursor
        if (result.alignment[i] != kAlignInsertionToQuery) {
            // TODO -- these memcpys could be merged
            std::memcpy(&feature_ptrs[kFeatureTemplateSignal]
                                     [stereo_global_cursor + template_segment_length],
                        &template_raw_data_ptr[template_signal_cursor], sizeof(SampleType));
            template_segment_length++;
            template_signal_cursor++;
            auto max_signal_length = template_moves_expanded.size();

            // We are relying on strings of 0s ended in a 1.  It would be more efficient
            // in any case to store run length data above.
            // We're also assuming uint8_t is an alias for char (not guaranteed in principle).
            const auto* const start_ptr = &template_moves_expanded[template_signal_cursor];
            auto* const next_move_ptr =
                    static_cast<const uint8_t*>(std::memchr(start_ptr, 1, max_signal_length));
            const size_t sample_count =
                    next_move_ptr ? (next_move_ptr - start_ptr) : max_signal_length;

            // Assumes contiguity of successive elements.
            std::memcpy(&feature_ptrs[kFeatureTemplateSignal]
                                     [stereo_global_cursor + template_segment_length],
                        &template_raw_data_ptr[template_signal_cursor],
                        sample_count * sizeof(SampleType));

            template_signal_cursor += sample_count;
            template_segment_length += sample_count;
        }

        // If there is *not* an insertion to the target, add the nucleotide from the query cursor
        if (result.alignment[i] != kAlignInsertionToTarget) {
            std::memcpy(&feature_ptrs[kFeatureComplementSignal]
                                     [stereo_global_cursor + complement_segment_length],
                        &flipped_complement_raw_data_ptr[complement_signal_cursor],
                        sizeof(SampleType));

            complement_segment_length++;
            complement_signal_cursor++;
            auto max_signal_length = complement_moves_expanded.size();

            // See comments above.
            const auto* const start_ptr = &complement_moves_expanded[complement_signal_cursor];
            auto* const next_move_ptr =
                    static_cast<const uint8_t*>(std::memchr(start_ptr, 1, max_signal_length));
            const size_t sample_count =
                    next_move_ptr ? (next_move_ptr - start_ptr) : max_signal_length;

            std::memcpy(&feature_ptrs[kFeatureComplementSignal]
                                     [stereo_global_cursor + complement_segment_length],
                        &flipped_complement_raw_data_ptr[complement_signal_cursor],
                        sample_count * sizeof(SampleType));

            complement_signal_cursor += sample_count;
            complement_segment_length += sample_count;
        }

        const int total_segment_length =
                std::max(template_segment_length, complement_segment_length);
        const int start_ts = stereo_global_cursor;

        // Converts Q scores from char to SampleType, with appropriate scale/offset.
        const auto convert_q_score = [](char q_in) {
            return static_cast<SampleType>(static_cast<float>(q_in - 33) / 90.0f);
        };

        // Now, add the nucleotides and q scores
        if (result.alignment[i] != kAlignInsertionToQuery) {
            const char nucleotide = template_read->seq[target_cursor];
            const auto nucleotide_feature_idx =
                    kFeatureTemplateFirstNucleotide + dorado::utils::base_to_int(nucleotide);
            std::fill_n(&feature_ptrs[nucleotide_feature_idx][start_ts], total_segment_length,
                        static_cast<SampleType>(1.0f));
            std::fill_n(&feature_ptrs[kFeatureTemplateQScore][start_ts], total_segment_length,
                        convert_q_score(template_read->qstring[target_cursor]));

            // Anything but a query insertion causes the target cursor to advance.
            ++target_cursor;
        }

        // Now, add the nucleotides and q scores
        if (result.alignment[i] != kAlignInsertionToTarget) {
            const char nucleotide = complement_sequence_reverse_complement.at(query_cursor);
            const auto nucleotide_feature_idx =
                    kFeatureComplementFirstNucleotide + dorado::utils::base_to_int(nucleotide);

            std::fill_n(&feature_ptrs[nucleotide_feature_idx][start_ts], total_segment_length,
                        static_cast<SampleType>(1.0f));
            std::fill_n(&feature_ptrs[kFeatureComplementQScore][start_ts], total_segment_length,
                        convert_q_score(complement_read->qstring.rbegin()[query_cursor]));

            // Anything but a target insertion causes the query cursor to advance.
            ++query_cursor;
        }

        feature_ptrs[kFeatureMoveTable][stereo_global_cursor] =
                static_cast<SampleType>(1);  // set the move table

        // Update the global cursor
        stereo_global_cursor += total_segment_length;
    }

    tmp = tmp.index(
            {torch::indexing::Slice(None), torch::indexing::Slice(None, stereo_global_cursor)});

    read->read_id = template_read->read_id + ";" + complement_read->read_id;
    read->read_tag = template_read->read_tag;
    read->raw_data = tmp;  // use the encoded signal
    read->is_duplex = true;
    read->run_id = template_read->run_id;

    edlibFreeAlignResult(result);

    return read;
}

void StereoDuplexEncoderNode::worker_thread() {
    Message message;
    while (m_work_queue.try_pop(message)) {
        if (std::holds_alternative<std::shared_ptr<ReadPair>>(message)) {
            auto read_pair = std::get<std::shared_ptr<ReadPair>>(message);
            std::shared_ptr<Read> stereo_encoded_read =
                    stereo_encode(read_pair->read_1, read_pair->read_2);

            if (stereo_encoded_read->raw_data.ndimension() ==
                2) {  // 2 dims for stereo encoding, 1 for simplex
                m_sink.push_message(
                        stereo_encoded_read);  // Stereo-encoded read created, send it to sink
            } else {
                // announce to downstream that we rejected a candidate pair
                --read_pair->read_1->num_duplex_candidate_pairs;
                m_sink.push_message(CandidatePairRejectedMessage{});
            }
        } else if (std::holds_alternative<std::shared_ptr<Read>>(message)) {
            auto read = std::get<std::shared_ptr<Read>>(message);
            m_sink.push_message(read);
        }
    }

    int num_worker_threads = --m_num_worker_threads;
    if (num_worker_threads == 0) {
        m_sink.terminate();
    }
}

StereoDuplexEncoderNode::StereoDuplexEncoderNode(MessageSink& sink, int input_signal_stride)
        : MessageSink(1000),
          m_sink(sink),
          m_num_worker_threads(std::thread::hardware_concurrency()),
          m_input_signal_stride(input_signal_stride) {
    for (int i = 0; i < m_num_worker_threads; i++) {
        std::unique_ptr<std::thread> stereo_encoder_worker_thread =
                std::make_unique<std::thread>(&StereoDuplexEncoderNode::worker_thread, this);
        worker_threads.push_back(std::move(stereo_encoder_worker_thread));
    }
}

StereoDuplexEncoderNode::~StereoDuplexEncoderNode() {
    terminate();
    for (auto& t : worker_threads) {
        t->join();
    }

    m_sink.terminate();
}

stats::NamedStats StereoDuplexEncoderNode::sample_stats() const {
    stats::NamedStats stats = m_work_queue.sample_stats();
    stats["discarded_pairs"] = m_num_discarded_pairs;
    return stats;
}

}  // namespace dorado
