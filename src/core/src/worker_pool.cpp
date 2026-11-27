#include "rummage/core/worker_pool.hpp"

#include <algorithm>

#include "rummage/core/log.hpp"
#include "rummage/core/memory.hpp"

namespace rummage::core {

WorkerPool::WorkerPool(EventLoop& loop, unsigned threads) : WorkerPool(loop, threads, false) {}

WorkerPool::WorkerPool(EventLoop& loop, unsigned threads, bool reclaim_on_idle)
    : loop_{loop}, reclaim_on_idle_{reclaim_on_idle} {
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

WorkerPool& WorkerPool::catalog_lane() {
    if (!catalog_lane_)
        catalog_lane_ = std::unique_ptr<WorkerPool>(new WorkerPool(loop_, 1, true));
    return *catalog_lane_;
}

WorkerPool& WorkerPool::preview_lane() {
    if (!preview_lane_)
        preview_lane_ = std::make_unique<WorkerPool>(loop_, 2);
    return *preview_lane_;
}

WorkerPool& WorkerPool::thumbnail_cache_lane() {
    if (!thumbnail_cache_lane_)
        thumbnail_cache_lane_ = std::make_unique<WorkerPool>(loop_, 1);
    return *thumbnail_cache_lane_;
}

void WorkerPool::enqueue(Priority priority, CancelToken token, std::function<void()> run) {
    Sleeper* next = nullptr;
    {
        const std::scoped_lock lock{mutex_};
        // Replaced searches and offscreen thumbnails must not retain closures behind
        // a slow filesystem operation. Running work remains cooperatively cancelled.
        for (auto& queue : queues_)
            std::erase_if(queue, [](const Job& job) { return job.token.cancelled(); });
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
    bool reclaim_pending = false;
    while (true) {
        Job job;
        {
            std::unique_lock lock{mutex_};
            while (!stopping_ &&
                   std::ranges::all_of(queues_, [](const auto& queue) { return queue.empty(); })) {
                idle_.push_back(&sleeper);
                const auto ready = [this, &sleeper] { return stopping_ || sleeper.notified; };
                if (reclaim_on_idle_ && reclaim_pending) {
                    // One shared catalog lane owns large JSON bursts. Reuse its arena during
                    // activity, then trim once after three uninterrupted idle seconds. This
                    // includes cancelled queries and survives their originating view closing.
                    if (!sleeper.wake.wait_for(lock, std::chrono::seconds{3}, ready)) {
                        std::erase(idle_, &sleeper);
                        reclaim_pending = false;
                        lock.unlock();
                        (void)give_back_free_memory_if_needed();
                        lock.lock();
                        continue;
                    }
                } else {
                    sleeper.wake.wait(lock, ready);
                }
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
        reclaim_pending = true;
    }
}

}  // namespace rummage::core
