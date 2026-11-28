#!/usr/bin/env python3
"""Disposable synthetic catalog scaling through the actual add-on ABI.

This measures catalog queries only, not import, decoding, or native UI responsiveness.
Run against a Debug or Release module and record which one was used.
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import platform
import resource
import sqlite3
import statistics
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("module", type=Path)
    parser.add_argument("--records", type=int, default=100_000)
    parser.add_argument("--samples", type=int, default=30)
    parser.add_argument("--build-mode", default="unspecified")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    if args.records < 100 or args.samples < 5:
        parser.error("use at least 100 records and five samples")
    module = ctypes.CDLL(str(args.module.resolve()))
    module.rummage_photos_request.argtypes = [ctypes.c_char_p, ctypes.c_char_p]
    module.rummage_photos_request.restype = ctypes.c_void_p
    module.rummage_photos_free.argtypes = [ctypes.c_void_p]
    with tempfile.TemporaryDirectory(prefix="rummage-photos-scale-") as temporary:
        root = Path(temporary)
        source = root / "originals"
        source.mkdir()
        catalog = root / "catalog/catalog.db"

        def request(value):
            response = module.rummage_photos_request(
                os.fsencode(catalog), json.dumps(value).encode()
            )
            if not response:
                raise RuntimeError("no catalog response")
            try:
                result = json.loads(ctypes.string_at(response))
            finally:
                module.rummage_photos_free(response)
            if not result.get("ok"):
                raise RuntimeError(result)
            return result

        source_id = request({"op": "add_source", "path": str(source)})["source"]["id"]
        started = time.perf_counter()
        with sqlite3.connect(catalog) as connection:
            connection.executemany(
                "INSERT INTO photos(source_id,relative_path,filename,device,inode,size,"
                "mtime,mtime_ns,description,rating,capture_date,metadata_signature) "
                "VALUES(?,?,?,?,?,?,?,?,?,?,?,?)",
                (
                    (
                        source_id,
                        f"folder-{index // 1000:03}/photo-{index:06}.jpg",
                        f"photo-{index:06}.jpg",
                        1,
                        index + 1,
                        1_000_000 + index,
                        1_700_000_000 + index,
                        0,
                        "coast holiday" if index % 5 == 0 else "mountain walking",
                        index % 6,
                        f"2024-{index % 12 + 1:02}-{index % 28 + 1:02}T12:30",
                        "synthetic",
                    )
                    for index in range(args.records)
                ),
            )
            connection.execute(
                "INSERT INTO photo_search(rowid,filename,description,tags) "
                "SELECT id,filename,description,'' FROM photos"
            )
        setup_ms = (time.perf_counter() - started) * 1000
        workloads = {
            "name_page": {},
            "filename_text": {"text": "photo-050"},
            "stored_text": {"text": "coast"},
            "date": {"date_from": "2024-06-01", "date_to": "2024-08-31", "sort": "date"},
            "rating": {"rating_min": 4, "sort": "rating"},
            "combined": {"text": "coast", "rating_min": 3, "date_from": "2024-01-01"},
        }
        results = {}
        for name, filters in workloads.items():
            request_value = {"op": "query", "limit": 200, **filters}
            first_started = time.perf_counter()
            result = request(request_value)
            first_ms = (time.perf_counter() - first_started) * 1000
            times = []
            for _ in range(args.samples):
                started = time.perf_counter()
                result = request(request_value)
                times.append((time.perf_counter() - started) * 1000)
                assert len(result["photos"]) <= 200
                assert len({item["id"] for item in result["photos"]}) == len(result["photos"])
            times.sort()
            results[name] = {
                "total": result["total"],
                "first_query_ms": first_ms,
                "p50_ms": statistics.median(times),
                "p95_ms": times[min(len(times) - 1, int(len(times) * 0.95))],
                "max_ms": max(times),
            }
        first = request({"op": "query", "limit": 200})
        second = request({"op": "query", "limit": 200, "offset": 200})
        assert not {row["id"] for row in first["photos"]}.intersection(
            row["id"] for row in second["photos"]
        )
        assert first["total"] == args.records
        cpu = next(
            (line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines() if line.startswith("model name")),
            "unknown",
        )
        report = {
            "scope": "synthetic catalog queries only; no real-image import or decoding measurement",
            "module": str(args.module.resolve()),
            "build_mode": args.build_mode,
            "machine": {"cpu": cpu, "kernel": platform.release(), "architecture": platform.machine()},
            "fixture_directory": str(root),
            "storage": "temporary directory; filesystem and storage device must be recorded separately",
            "cache": "warm filesystem cache; first query is not a cold-storage measurement",
            "records": args.records,
            "samples_per_workload": args.samples,
            "page_size": 200,
            "populate_ms": setup_ms,
            "peak_rss_kib": int(next(line.split()[1] for line in Path("/proc/self/status").read_text().splitlines() if line.startswith("VmHWM:"))),
            "lifetime_peak_rss_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            "queries": results,
        }
        encoded = json.dumps(report, indent=2) + "\n"
        print(encoded, end="")
        if args.output:
            args.output.write_text(encoded)


if __name__ == "__main__":
    main()
