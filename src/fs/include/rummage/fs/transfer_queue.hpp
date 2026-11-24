#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

#include "rummage/core/event_loop.hpp"
#include "rummage/fs/transfer.hpp"

namespace rummage::fs {

enum class TransferState { queued, running, paused, waiting, completed, failed, cancelled };

struct TransferTask {
    std::uint64_t id{0};
    TransferRequest request;
    TransferProgress progress;
    TransferState state{TransferState::queued};
    bool started{false};
    bool pausable{false};
    std::vector<std::string> errors;
    std::size_t error_count{0};

    [[nodiscard]] bool finished() const {
        return state == TransferState::completed || state == TransferState::failed ||
               state == TransferState::cancelled;
    }
};

// FIFO queue on a dedicated thread. IDs stay stable through progress, cancellation and
// completion. All public methods and callbacks run on the event-loop thread. Workers poll
// cooperative pause checkpoints only for ordinary filesystem copy/move jobs.
class TransferQueue {
public:
    static constexpr std::chrono::milliseconds kProgressInterval{100};
    static constexpr std::size_t kHistoryLimit = 100;
    using Progress = std::function<void(const TransferRequest&, const TransferProgress&)>;
    using Ask = std::function<void(const Conflict&, std::function<void(Resolution)>)>;
    using Done = std::function<void(const TransferRequest&, const TransferResult&)>;
    TransferQueue(core::EventLoop&, Progress, Ask, Done, std::function<void()> changed = {});
    ~TransferQueue();
    TransferQueue(const TransferQueue&) = delete;
    TransferQueue& operator=(const TransferQueue&) = delete;
    TransferQueue(TransferQueue&&) = delete;
    TransferQueue& operator=(TransferQueue&&) = delete;

    std::uint64_t submit(TransferRequest request);
    void cancel();
    bool cancel(std::uint64_t id);
    bool pause(std::uint64_t id, bool paused);
    void clear_finished();
    [[nodiscard]] std::vector<TransferTask> tasks() const;

    [[nodiscard]] std::size_t pending() const noexcept { return pending_; }

private:
    struct Job;
    struct Shared;
    void work();
    void complete(const std::shared_ptr<Job>&, TransferResult);
    void changed();
    core::EventLoop& loop_;
    Progress progress_;
    Ask ask_;
    Done done_;
    std::function<void()> changed_;
    std::size_t pending_{0};
    std::uint64_t next_id_{0};
    std::shared_ptr<Shared> shared_;
    std::shared_ptr<bool> alive_{std::make_shared<bool>(true)};
    std::thread thread_;
};
}  // namespace rummage::fs
