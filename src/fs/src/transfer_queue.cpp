#include "rummage/fs/transfer_queue.hpp"

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "rummage/core/i18n.hpp"
#include "rummage/fs/archive_folder.hpp"
#include "rummage/fs/compress.hpp"
#include "rummage/fs/erase.hpp"
#include "rummage/fs/extract.hpp"
#include "rummage/fs/group.hpp"
#include "rummage/fs/links.hpp"
#include "rummage/fs/trash.hpp"

namespace rummage::fs {
struct TransferQueue::Job {
    TransferTask task;
    core::CancelToken token;
    bool paused{false};
    bool asking{false};
    bool finishing{false};
    std::uint64_t question{0};
    std::optional<Resolution> answer;
    std::chrono::steady_clock::time_point last{};
};

struct TransferQueue::Shared {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> queue, history;
    std::shared_ptr<Job> current;
    bool stopping{false};
};

TransferQueue::TransferQueue(core::EventLoop& loop, Progress progress, Ask ask, Done done,
                             std::function<void()> changed)
    : loop_{loop},
      progress_{std::move(progress)},
      ask_{std::move(ask)},
      done_{std::move(done)},
      changed_{std::move(changed)},
      shared_{std::make_shared<Shared>()},
      thread_{[this] { work(); }} {}

TransferQueue::~TransferQueue() {
    *alive_ = false;
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->stopping = true;
        shared_->queue.clear();
        if (shared_->current)
            shared_->current->token.cancel();
    }
    shared_->wake.notify_all();
    thread_.join();
}

void TransferQueue::changed() {
    if (changed_)
        changed_();
}

std::uint64_t TransferQueue::submit(TransferRequest request) {
    auto job = std::make_shared<Job>();
    request.id = ++next_id_;
    job->task.id = request.id;
    job->task.request = std::move(request);
    job->task.pausable = (job->task.request.kind == TransferKind::copy ||
                          job->task.request.kind == TransferKind::move) &&
                         !job->task.request.from_archive;
    ++pending_;
    {
        const std::scoped_lock lock{shared_->mutex};
        shared_->history.push_back(job);
        shared_->queue.push_back(job);
    }
    shared_->wake.notify_all();
    changed();
    return job->task.id;
}

std::vector<TransferTask> TransferQueue::tasks() const {
    const std::scoped_lock lock{shared_->mutex};
    std::vector<TransferTask> result;
    result.reserve(shared_->history.size());
    for (const auto& job : shared_->history)
        result.push_back(job->task);
    return result;
}

void TransferQueue::clear_finished() {
    {
        const std::scoped_lock lock{shared_->mutex};
        std::erase_if(shared_->history, [](const auto& job) { return job->task.finished(); });
    }
    changed();
}

void TransferQueue::cancel() {
    std::uint64_t id = 0;
    {
        const std::scoped_lock lock{shared_->mutex};
        if (shared_->current)
            id = shared_->current->task.id;
        else if (!shared_->queue.empty())
            id = shared_->queue.front()->task.id;
    }
    if (id)
        cancel(id);
}

bool TransferQueue::cancel(std::uint64_t id) {
    std::shared_ptr<Job> cancelled;
    {
        const std::scoped_lock lock{shared_->mutex};
        const auto found = std::ranges::find_if(
            shared_->history, [id](const auto& job) { return job->task.id == id; });
        if (found == shared_->history.end() || (*found)->task.finished() || (*found)->finishing ||
            (*found)->token.cancelled())
            return false;
        auto job = *found;
        job->token.cancel();
        job->paused = false;
        if (!job->task.started) {
            std::erase(shared_->queue, job);
            cancelled = job;
        }
    }
    shared_->wake.notify_all();
    if (cancelled) {
        TransferResult result;
        result.cancelled = true;
        result.cancelled_before_start = true;
        complete(cancelled, std::move(result));
    } else
        changed();
    return true;
}

bool TransferQueue::pause(std::uint64_t id, bool paused) {
    {
        const std::scoped_lock lock{shared_->mutex};
        const auto found = std::ranges::find_if(
            shared_->history, [id](const auto& job) { return job->task.id == id; });
        if (found == shared_->history.end())
            return false;
        auto& job = **found;
        if (job.task.finished() || job.finishing || !job.task.pausable || job.token.cancelled())
            return false;
        job.paused = paused;
        job.task.state = paused              ? TransferState::paused
                         : !job.task.started ? TransferState::queued
                         : job.asking        ? TransferState::waiting
                                             : TransferState::running;
    }
    shared_->wake.notify_all();
    changed();
    return true;
}

void TransferQueue::complete(const std::shared_ptr<Job>& job, TransferResult result) {
    // Always delivered on the loop, including cancellations before the worker starts.
    loop_.post([this, alive = alive_, shared = shared_, job, result = std::move(result)] {
        if (!*alive)
            return;
        TransferRequest request;
        {
            const std::scoped_lock lock{shared->mutex};
            job->task.state = result.cancelled ? TransferState::cancelled
                              : (!result.refused.empty() || !result.errors.empty())
                                  ? TransferState::failed
                                  : TransferState::completed;
            job->task.progress.files_done = result.files_done;
            job->task.progress.bytes_done = result.bytes_done;
            job->task.progress.files_verified = result.files_verified;
            job->task.error_count = result.errors.size() + (result.refused.empty() ? 0U : 1U);
            if (!result.refused.empty())
                job->task.errors.push_back(result.refused);
            for (const auto& error : result.errors) {
                if (job->task.errors.size() == 64)
                    break;
                job->task.errors.push_back(error);
            }
            request = job->task.request;
            auto finished = std::ranges::count_if(
                shared->history, [](const auto& item) { return item->task.finished(); });
            for (auto it = shared->history.begin();
                 it != shared->history.end() &&
                 finished > static_cast<std::ptrdiff_t>(kHistoryLimit);) {
                if ((*it)->task.finished()) {
                    it = shared->history.erase(it);
                    --finished;
                } else
                    ++it;
            }
        }
        --pending_;
        if (done_)
            done_(request, result);
        changed();
    });
}

void TransferQueue::work() {
    const auto shared = shared_;
    for (;;) {
        std::shared_ptr<Job> job;
        {
            std::unique_lock lock{shared->mutex};
            shared->wake.wait(lock, [&] {
                return shared->stopping ||
                       (!shared->queue.empty() && !shared->queue.front()->paused);
            });
            if (shared->stopping)
                return;
            job = shared->queue.front();
            shared->queue.pop_front();
            shared->current = job;
            job->task.started = true;
            job->task.state = TransferState::running;
        }
        auto request = job->task.request;
        if ((request.kind == TransferKind::copy || request.kind == TransferKind::move) &&
            find_archive(request.source)) {
            request.from_archive = true;
            const std::scoped_lock lock{shared->mutex};
            job->task.request.from_archive = true;
            job->task.pausable = false;
            job->paused = false;
            job->task.state = TransferState::running;
        }
        loop_.post([this, alive = alive_] {
            if (*alive)
                changed();
        });
        TransferCallbacks callbacks;
        callbacks.checkpoint = [shared, job] {
            std::unique_lock lock{shared->mutex};
            shared->wake.wait(
                lock, [&] { return shared->stopping || job->token.cancelled() || !job->paused; });
        };
        callbacks.progress = [this, shared, job, request](const TransferProgress& progress) {
            const auto now = std::chrono::steady_clock::now();
            {
                const std::scoped_lock lock{shared->mutex};
                job->task.progress = progress;
                if (now - job->last < kProgressInterval)
                    return;
                job->last = now;
            }
            loop_.post([this, alive = alive_, request, progress] {
                if (!*alive)
                    return;
                if (progress_)
                    progress_(request, progress);
                changed();
            });
        };
        callbacks.resolve = [this, shared, job](const Conflict& conflict) {
            std::uint64_t question = 0;
            {
                const std::scoped_lock lock{shared->mutex};
                if (shared->stopping || job->token.cancelled())
                    return Resolution::cancel;
                question = ++job->question;
                job->asking = true;
                job->answer.reset();
                job->task.state = job->paused ? TransferState::paused : TransferState::waiting;
            }
            loop_.post([this, alive = alive_, shared, job, conflict, question] {
                if (!*alive)
                    return;
                {
                    const std::scoped_lock lock{shared->mutex};
                    if (job->token.cancelled() || !job->asking || job->question != question)
                        return;
                }
                changed();
                const auto reply = [shared, job, question](Resolution resolution) {
                    {
                        const std::scoped_lock lock{shared->mutex};
                        if (!job->asking || job->question != question || job->token.cancelled())
                            return;
                        job->answer = resolution;
                        if (resolution == Resolution::cancel) {
                            job->token.cancel();
                            job->paused = false;
                        }
                        job->asking = false;
                        job->task.state =
                            job->paused ? TransferState::paused : TransferState::running;
                    }
                    shared->wake.notify_all();
                };
                if (ask_)
                    ask_(conflict, reply);
                else
                    reply(Resolution::cancel);
            });
            std::unique_lock lock{shared->mutex};
            shared->wake.wait(lock, [&] {
                return shared->stopping || job->token.cancelled() ||
                       (job->answer.has_value() && !job->paused);
            });
            job->asking = false;
            if (shared->stopping || job->token.cancelled())
                return Resolution::cancel;
            return *job->answer;
        };
        TransferResult result;
        switch (request.kind) {
            case TransferKind::group:
                result = run_group(request, job->token, callbacks);
                break;
            case TransferKind::link:
                result = run_link(request, job->token, callbacks);
                break;
            case TransferKind::compress:
                result = run_compress(request, job->token, callbacks);
                break;
            case TransferKind::trash:
                result = run_trash(request, job->token, callbacks);
                break;
            case TransferKind::restore:
                result = run_restore(request, job->token, callbacks);
                break;
            case TransferKind::erase:
            case TransferKind::empty_trash:
                result = run_erase(request, job->token, callbacks);
                break;
            case TransferKind::copy:
            case TransferKind::move:
                if (request.from_archive) {
                    if (request.verify)
                        result.refused =
                            i18n::tr("Copy verification is unavailable for archive extraction");
                    else if (request.kind == TransferKind::move)
                        result.refused = "Archive contents are read-only; copy them instead";
                    else
                        result = run_extract(request, job->token, callbacks);
                } else
                    result = run_transfer(request, job->token, callbacks);
                break;
        }
        {
            const std::scoped_lock lock{shared->mutex};
            job->finishing = true;
            shared->current.reset();
        }
        complete(job, std::move(result));
    }
}
}  // namespace rummage::fs
