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

        def version_two(catalog):
            call(catalog, {"op": "create_album", "name": "Preserved album"})
            with sqlite3.connect(catalog) as connection:
                connection.executescript(
                    "DROP TABLE photo_receipts; ALTER TABLE sources DROP COLUMN watch_enabled; "
                    "DROP INDEX photos_pending_metadata; DROP INDEX photos_seen_job; "
                    "DROP INDEX photos_modified; PRAGMA user_version=2;"
                )

        def version_one(catalog):
            version_two(catalog)
            with sqlite3.connect(catalog) as connection:
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
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 3
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
        old_backup = Path(str(interrupted) + ".before-schema-2.backup")
        assert old_backup.exists()
        original_backup = old_backup.read_bytes()
        with sqlite3.connect(interrupted) as connection:
            connection.execute("DROP TABLE source_directories")
        assert call(interrupted, {"op": "albums"})["albums"][0]["name"] == "Preserved album"
        assert old_backup.read_bytes() == original_backup
        assert Path(str(old_backup) + ".1").exists()

        two = root / "version-two" / "catalog.db"
        version_two(two)
        assert call(two, {"op": "albums"})["albums"][0]["name"] == "Preserved album"
        with sqlite3.connect(two) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 3
            assert connection.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
        assert Path(str(two) + ".before-schema-3.backup").exists()

        # A schema-two operation journal must rebuild its indexed trash receipts.
        receipt_catalog = root / "receipt-upgrade" / "catalog.db"
        pictures = root / "receipt-pictures"
        pictures.mkdir()
        (pictures / "keep.jpg").write_bytes(b"same bytes")
        removed_file = pictures / "remove.jpg"
        removed_file.write_bytes(b"same bytes")
        call(receipt_catalog, {"op": "add_source", "path": str(pictures)})
        call(receipt_catalog, {"op": "work", "budget": 64})
        photos = call(receipt_catalog, {"op": "query"})["photos"]
        keeper = next(photo for photo in photos if photo["filename"] == "keep.jpg")
        removed = next(photo for photo in photos if photo["filename"] == "remove.jpg")
        intent = call(receipt_catalog, {"op": "operation_prepare", "kind": "trash", "ids": [removed["id"]], "keeper_ids": [keeper["id"]]})
        receipt_path = root / "disposable-trash-copy.jpg"
        removed_file.rename(receipt_path)
        call(receipt_catalog, {"op": "operation_complete", "operation_id": intent["operation_id"], "results": [{"id": removed["id"], "success": True, "destination": str(receipt_path)}]})
        with sqlite3.connect(receipt_catalog) as connection:
            connection.executescript(
                "DROP TABLE photo_receipts; ALTER TABLE sources DROP COLUMN watch_enabled; "
                "DROP INDEX photos_pending_metadata; DROP INDEX photos_seen_job; "
                "DROP INDEX photos_modified; PRAGMA user_version=2;"
            )
        restored_receipt = call(receipt_catalog, {"op": "operation_restore_prepare", "ids": [removed["id"]]})
        assert restored_receipt["photos"][0]["path"] == str(receipt_path)
        assert restored_receipt["photos"][0]["id"] == removed["id"]
        with sqlite3.connect(receipt_catalog) as connection:
            assert connection.execute("SELECT path FROM photo_receipts WHERE photo_id=?", (removed["id"],)).fetchone()[0] == str(receipt_path)
            assert connection.execute("PRAGMA foreign_key_check").fetchall() == []

        blocked = root / "blocked" / "catalog.db"
        version_one(blocked)
        existing = Path(str(blocked) + ".before-schema-2.backup")
        existing.write_text("existing backup must not be overwritten")
        call(blocked, {"op": "albums"})
        assert Path(str(existing) + ".1").exists()
        assert existing.read_text() == "existing backup must not be overwritten"
        with sqlite3.connect(blocked) as connection:
            assert connection.execute("PRAGMA user_version").fetchone()[0] == 3
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
        # Exact nanosecond bounds and out-of-range values use synthetic stat columns,
        # avoiding filesystem-specific timestamp clamps while exercising the real ABI.
        bounds = root / "timestamp-bounds" / "catalog.db"
        sid = call(bounds, {"op": "add_source", "path": str(source)})["source"]["id"]
        with sqlite3.connect(bounds) as connection:
            connection.execute("INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns) VALUES(?,'bound.jpg','bound.jpg',1,1,1,0,0)", (sid,))
        for seconds, fraction, expected in [
            (9223372036, 854775807, 2**63-1),
            (-9223372037, 145224192, -(2**63)),
            (9223372036, 854775808, None),
            (-9223372037, 145224191, None),
            (10000000000, 0, None),
        ]:
            with sqlite3.connect(bounds) as connection:
                connection.execute("UPDATE photos SET mtime=?,mtime_ns=?", (seconds, fraction))
            photo = call(bounds, {"op": "query"})["photos"][0]
            assert photo["mtime_ns_valid"] == (expected is not None)
            assert photo["mtime_ns"] == (expected if expected is not None else 0)

        # More than the legacy 1000 groups must remain reachable through pagination.
        groups = root / "duplicate-pages" / "catalog.db"
        sid = call(groups, {"op": "add_source", "path": str(source)})["source"]["id"]
        with sqlite3.connect(groups) as connection:
            connection.executemany("INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,fingerprint) VALUES(?,?,?,?,?,1,0,0,?)", ((sid, f"{group}-{copy}.jpg", f"{group}-{copy}.jpg", 1, group*2+copy+1, f"hash-{group:04}") for group in range(1001) for copy in range(2)))
        first = call(groups, {"op": "duplicates", "limit": 1000})
        last = call(groups, {"op": "duplicates", "offset": 1000, "limit": 1000})
        assert first["total"] == 1001 and first["more"] is True
        assert len(first["groups"]) == 1000 and len(last["groups"]) == 1
        assert last["more"] is False
        assert first["groups"][-1]["key"] != last["groups"][0]["key"]

        # Re-adding a source repairs legacy malformed settings without deleting annotations.
        with sqlite3.connect(bounds) as connection:
            connection.execute("UPDATE sources SET exclusions='[7]',recursive=1")
        repaired = call(bounds, {"op": "add_source", "path": str(source), "exclusions": [], "recursive": False})
        assert repaired["source"]["exclusions"] == [] and repaired["source"]["recursive"] is False
        call(bounds, {"op": "work"})
        with sqlite3.connect(bounds) as connection:
            assert connection.execute("SELECT count(*) FROM photos").fetchone()[0] == 1

        # Progress reports aggregate state counts without replaying full job history.
        with sqlite3.connect(groups) as connection:
            connection.executemany("INSERT INTO jobs(kind,state) VALUES('duplicates',?)", [("completed",)] * 1001 + [("paused",)] * 4 + [("waiting",)] * 2 + [("failed",)] * 3)
        progress = call(groups, {"op": "progress"})
        assert len(progress["jobs"]) == 1 and progress["jobs"][0]["state"] == "queued"
        assert progress["job_counts"] == {"paused": 4, "waiting": 2, "failed": 3}

        # Indexed summary counts must agree with detailed history for all metadata
        # states, including unavailable records, claims, and an empty scan.
        counters = root / "progress-counts" / "catalog.db"
        added = call(counters, {"op": "add_source", "path": str(source)})
        sid, job = added["source"]["id"], added["job_id"]
        states = [("online", "done"), ("online", ""),
                  ("unreadable", "done"), ("unreadable", ""),
                  ("changed", ""), ("missing", "done"), ("replaced", "")]
        with sqlite3.connect(counters) as connection:
            connection.executemany("INSERT INTO photos(source_id,relative_path,filename,device,inode,size,mtime,mtime_ns,availability,metadata_signature,seen_job) VALUES(?,?,?,1,?,1,0,0,?,?,?)",
                ((sid, f"state-{i}.jpg", f"state-{i}.jpg", i+1, availability, signature, job)
                 for i, (availability, signature) in enumerate(states)))
            connection.execute("INSERT INTO metadata_claims(photo_id,token,owner,owner_start,expires) VALUES(2,'test','','',unixepoch()+3600)")
        summary = call(counters, {"op": "progress"})["jobs"][0]
        detailed = call(counters, {"op": "jobs"})["jobs"][0]
        keys = ["metadata_completed", "metadata_pending", "metadata_total",
                "metadata_unavailable", "pending_directories", "in_flight", "phase"]
        assert {key: summary[key] for key in keys} == {key: detailed[key] for key in keys}
        assert (summary["metadata_completed"], summary["metadata_pending"], summary["metadata_unavailable"], summary["in_flight"]) == (2, 1, 4, 1)
        with sqlite3.connect(counters) as connection:
            connection.execute("UPDATE jobs SET state='completed'")
            connection.execute("INSERT INTO jobs(kind,source_id,state) VALUES('scan',?,'queued')", (sid,))
        summary = call(counters, {"op": "progress"})["jobs"][0]
        detailed = call(counters, {"op": "jobs"})["jobs"][0]
        assert {key: summary[key] for key in keys} == {key: detailed[key] for key in keys}
        assert summary["metadata_total"] == summary["metadata_unavailable"] == summary["in_flight"] == 0

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
    print("Photo catalog migration: schema-one/two upgrade, preserved unique backups, failed-upgrade retry, rollback and concurrent initialization passed")


if __name__ == "__main__":
    main()
