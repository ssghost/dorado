#include "ReadFilterNode.h"

#include "utils/sequence_utils.h"

#include <spdlog/spdlog.h>

namespace dorado {

void ReadFilterNode::worker_thread() {
    m_active_threads++;

    Message message;
    while (m_work_queue.try_pop(message)) {
        if (std::holds_alternative<CandidatePairRejectedMessage>(message)) {
            // discard, nothing downstream of this node is interested in this message
            continue;
        }

        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<std::shared_ptr<Read>>(message);

        // Filter based on qscore.
        if ((utils::mean_qscore_from_qstring(read->qstring) < m_min_qscore) ||
            read->seq.size() < m_min_read_length) {
            ++m_num_reads_filtered;
        } else if (m_read_ids_to_filter.find(read->read_id) != m_read_ids_to_filter.end()) {
            ++m_num_reads_filtered;
        } else {
            m_sink.push_message(read);
        }
    }

    auto num_active_threads = --m_active_threads;
    if (num_active_threads == 0) {
        m_sink.terminate();
    }
}

ReadFilterNode::ReadFilterNode(MessageSink& sink,
                               size_t min_qscore,
                               size_t min_read_length,
                               const std::unordered_set<std::string>& read_ids_to_filter,
                               size_t num_worker_threads)
        : MessageSink(1000),
          m_sink(sink),
          m_min_qscore(min_qscore),
          m_min_read_length(min_read_length),
          m_read_ids_to_filter(std::move(read_ids_to_filter)),
          m_num_reads_filtered(0),
          m_active_threads(0) {
    for (size_t i = 0; i < num_worker_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&ReadFilterNode::worker_thread, this)));
    }
}

ReadFilterNode::~ReadFilterNode() {
    terminate();
    for (auto& m : m_workers) {
        m->join();
    }
    m_sink.terminate();
}

stats::NamedStats ReadFilterNode::sample_stats() const {
    stats::NamedStats stats = stats::from_obj(m_work_queue);
    stats["reads_filtered"] = m_num_reads_filtered;
    return stats;
}

}  // namespace dorado
