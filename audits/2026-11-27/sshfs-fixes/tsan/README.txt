Final cache-lane targeted ThreadSanitizer validation, 2026-11-27.

Intentional-race control detected its data race and exited66. Actual current EventLoop, WorkerPool (including catalog, preview and thumbnail-cache lanes), memory and log implementations plus all existing worker/event-loop tests, prior cancellation/posting/destruction stress, and scratch-only catalog idle-wake/retired-owner stress passed three randomized runs:26cases,46,215assertions each, zero sanitizer diagnostics, exit0. Compiled source and all core header hashes remained unchanged after all runs.

Command: python3 /tmp/rummage-cachelane-tsan-n_zkocp0/run.py --window-authorized

Build is isolated here; no shared build files or production source edits. Exact compiler, flags, commands, source/header hashes, results and durations are recorded in results.json. No suppressions. TSan validates instrumented concurrency/lifetime paths; this is not whole-application coverage or a physical memory-reclamation benchmark (TSan replaces the allocator).
