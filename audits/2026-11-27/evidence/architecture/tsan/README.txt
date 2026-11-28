# Targeted ThreadSanitizer harness results

Status: executed successfully after the parent authorized the post-benchmark window. The intentional-race control produced the expected TSan report and exit 66. All three core runs (random seeds 731, 732, 733) passed 21 cases and 46,199 assertions each with exit 0 and no sanitizer diagnostics.

Inputs: unchanged frozen EventLoop, WorkerPool and logging sources; unchanged worker_pool_test.cpp/event_loop_test.cpp (18 cases, including header-only WindowSession lifetime cases); scratch-only doctest main and three extra stress cases. No renderer, decoder, SQLite, generated catalogs, full core library, production modifications, or shared-build output.

Additional coverage: concurrent cancellation of four streaming workers over 20 rounds; eight EventLoop producers, 4096 callbacks/round over 10 rounds; destroy pools with queued streaming callbacks holding subsequently destroyed owners over 20 rounds. Submission and pool destruction stay on the loop thread, matching the public contract; only cancellation/posting run concurrently.

Run, only after parent authorizes:

```
python3 /tmp/rummage-branch-audit-2026-11-27/architecture/tsan/run.py --window-authorized
```

The driver first compiles/runs an intentional data race to establish that TSan is active. Expected control exit 66 plus `WARNING: ThreadSanitizer: data race`. Any startup mapping/runtime failure is recorded and aborts further work; it is not interpreted as a clean sanitizer result. Only after the control succeeds are the actual sources compiled with `-fsanitize=thread -O1 -g -pthread -fno-omit-frame-pointer` and tested three times with randomized order. No ASan preload or suppression lists. All sources, compiler/options, SHA256 hashes, commands, exits and durations go to results.json/logs here.

This is targeted race/lifetime coverage, not whole-application TSan coverage. Library/system code not instrumented by the selected toolchain remains outside its reach. No user catalog/data is read.
