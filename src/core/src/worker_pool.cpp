#include "rummage/core/worker_pool.hpp"

#include <algorithm>

#include "rummage/core/log.hpp"

namespace rummage::core {

WorkerPool::WorkerPool(EventLoop& loop, unsigned threads) : loop_{loop} {
    if (threads == 0) {
        threads = std::max(2U, std::thread::hardware_concurrency() / 2);
    }
    threads_.reserve(threads);
    sleepers_.reserve(threads);
    idle_.reserve(threads);
    for (unsigned i = 0; i < threads; ++i) {
        auto& sleeper = *sleepers_.emplace_back(std::make_unique<Sleeper>());
        threads_.emplace_back([this, &sleeper] { worker_main(sleeper); });
    }
    log::debug("worker pool: {} threads", threads);
}

WorkerPool::~WorkerPool() {
    alive_->store(false);
    {
        const std::scoped_lock lock{mutex_};
        stopping_ = true;
        for (auto& queue : queues_) {
            for (const Job& job : queue) {
                job.token.cancel();
            }
            queue.clear();
        }
    }
    for (const auto& sleeper : sleepers_) {
        sleeper->wake.notify_one();
    }
    for (std::thread& thread : threads_) {
        thread.join();
    }
}

void WorkerPool::enqueue(Priority priority, CancelToken token, std::function<void()> run) {
    Sleeper* next = nullptr;
    {
        const std::scoped_lock lock{mutex_};
        queues_.at(static_cast<std::size_t>(priority))
            .push_back({.token = std::move(token), .run = std::move(run)});
        if (!idle_.empty()) {
            next = idle_.back();
            idle_.pop_back();
            next->notified = true;
        }
    }
    if (next != nullptr) {
        next->wake.notify_one();
    }
}

void WorkerPool::worker_main(Sleeper& sleeper) {
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            while (!stopping_ &&
                   std::ranges::all_of(queues_, [](const auto& queue) { return queue.empty(); })) {
                idle_.push_back(&sleeper);
                sleeper.wake.wait(lock, [this, &sleeper] { return stopping_ || sleeper.notified; });
                sleeper.notified = false;
            }
            if (stopping_) {
                return;
            }
            for (auto& queue : queues_) {  // highest priority first
                if (!queue.empty()) {
                    job = std::move(queue.front());
                    queue.pop_front();
                    break;
                }
            }
        }
        job.run();
    }
}

}  // namespace rummage::core
