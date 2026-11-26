#!/usr/bin/env python3
"""Exercise version-one catalog upgrades and failure recovery through the add-on ABI."""
import argparse
import ctypes
import concurrent.futures
import multiprocessing
import json
from pathlib import Path
import sqlite3
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    args = parser.parse_args()
    module = ctypes.CDLL(str(args.module.resolve()))
    module.rummage_photos_request.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    module.rummage_photos_request.restype = ctypes.c_void_p
    module.rummage_photos_free.argtypes = [ctypes.c_void_p]
    with tempfile.TemporaryDirectory(prefix="rummage-photos-migration-") as temporary:
        root = Path(temporary)

        def call(catalog, request, success=True):
            pointer = module.rummage_photos_request(str(catalog).encode(), json.dumps(request).encode())
            assert pointer, "catalog returned no response"
            try:
                result = json.loads(ctypes.string_at(pointer))
            finally:
                module.rummage_photos_free(pointer)
            assert result["ok"] == success, result
            return result

        def version_one(catalog):
            call(catalog, {"op": "create_album", "name": "Preserved album"})
            with sqlite3.connect(catalog) as connection:
                # Precisely remove the schema-two additions, retaining the real schema-one
                # catalog and user tables produced through the application boundary.
                connection.executescript(
                    "DROP TABLE source_directories; DROP TABLE background_settings; "
                    "ALTER TABLE jobs DROP COLUMN refresh_again; PRAGMA user_version=1;"
                )

        catalog = root / "good" / "catalog.db"
        version_one(catalog)
        assert call(catalog, {"op": "albums"})["albums"][0]["name"] == "Preserved album"
        assert call(catalog, {"op": "background_state"})["paused"] is False
        backup = Path(str(catalog) + ".before-schema-2.backup")
        assert backup.exists()
        with sqlite3.connect(backup) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 1
            assert connection.execute("SELECT name FROM albums").fetchone()[0] == "Preserved album"
            assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        with sqlite3.connect(catalog) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 2
            assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"

        interrupted = root / "interrupted" / "catalog.db"
        version_one(interrupted)
        with sqlite3.connect(interrupted) as connection:
            # Force a migration error after ALTER TABLE and CREATE TABLE have run.
            connection.execute("CREATE TABLE source_directories(unexpected INTEGER)")
        error = call(interrupted, {"op": "albums"}, success=False)["error"]
        assert "upgrade failed" in error and "backup" in error
        with sqlite3.connect(interrupted) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 1
            assert "refresh_again" not in {row[1] for row in connection.execute("PRAGMA table_info(jobs)")}
            assert connection.execute("SELECT count(*) FROM sqlite_master WHERE name='background_settings'").fetchone()[0] == 0
            assert connection.execute("SELECT name FROM albums").fetchone()[0] == "Preserved album"
        assert Path(str(interrupted) + ".before-schema-2.backup").exists()

        blocked = root / "blocked" / "catalog.db"
        version_one(blocked)
        existing = Path(str(blocked) + ".before-schema-2.backup")
        existing.write_text("existing backup must not be overwritten")
        call(blocked, {"op": "albums"}, success=False)
        assert existing.read_text() == "existing backup must not be overwritten"
        with sqlite3.connect(blocked) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 1
            assert connection.execute("SELECT name FROM albums").fetchone()[0] == "Preserved album"
        source = root / "source"
        source.mkdir()
        operations = [{"op": "sources"}, {"op": "add_source", "path": str(source)}, {"op": "jobs"}, {"op": "operations"}]
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            for iteration in range(30):
                fresh = root / f"concurrent-{iteration}" / "catalog.db"
                futures = [pool.submit(call, fresh, operation) for operation in operations]
                for future in futures:
                    future.result()
        context = multiprocessing.get_context("fork")
        for upgrade in (False, True):
            concurrent_catalog = root / ("process-upgrade" if upgrade else "process-fresh") / "catalog.db"
            if upgrade:
                version_one(concurrent_catalog)
            queue = context.Queue()
            def request_in_process(operation):
                try:
                    call(concurrent_catalog, operation)
                    queue.put(None)
                except Exception as error:
                    queue.put(str(error))
            processes = [context.Process(target=request_in_process, args=(operation,)) for operation in operations]
            for process in processes:
                process.start()
            failures = [queue.get(timeout=15) for _ in processes]
            for process in processes:
                process.join(timeout=15)
                if process.is_alive():
                    process.terminate()
                    raise AssertionError("Concurrent catalog initialization hung")
                assert process.exitcode == 0
            assert not any(failures), failures
    print("Photo catalog migration: upgrade, backup, rollback, refusal and concurrent thread/process initialization passed")


if __name__ == "__main__":
    main()
