#include "ReadToBamTypeNode.h"

#include <spdlog/spdlog.h>

#include <chrono>

namespace dorado {

void ReadToBamType::worker_thread() {
    m_active_threads++;

    Message message;
    while (m_work_queue.try_pop(message)) {
        // If this message isn't a read, we'll get a bad_variant_access exception.
        auto read = std::get<std::shared_ptr<Read>>(message);

        if (m_rna) {
            std::reverse(read->seq.begin(), read->seq.end());
            std::reverse(read->qstring.begin(), read->qstring.end());
        }

        auto alns = read->extract_sam_lines(m_emit_moves, m_modbase_threshold);
        for (auto& aln : alns) {
            m_sink.push_message(std::move(aln));
        }
    }

    auto num_active_threads = --m_active_threads;
    if (num_active_threads == 0) {
        m_sink.terminate();
    }
}

ReadToBamType::ReadToBamType(MessageSink& sink,
                             bool emit_moves,
                             bool rna,
                             size_t num_worker_threads,
                             float modbase_threshold_frac,
                             size_t max_reads)
        : MessageSink(max_reads),
          m_sink(sink),
          m_emit_moves(emit_moves),
          m_rna(rna),
          m_modbase_threshold(
                  static_cast<uint8_t>(std::min(modbase_threshold_frac * 256.0f, 255.0f))),
          m_active_threads(0) {
    for (size_t i = 0; i < num_worker_threads; i++) {
        m_workers.push_back(
                std::make_unique<std::thread>(std::thread(&ReadToBamType::worker_thread, this)));
    }
}

ReadToBamType::~ReadToBamType() {
    terminate();
    for (auto& m : m_workers) {
        m->join();
    }
    m_sink.terminate();
}

}  // namespace dorado
