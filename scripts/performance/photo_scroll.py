#!/usr/bin/env python3
"""Private-display, read-only-source Photo Library scrolling/memory experiment.

Run under display.py. Uses benchmark-only Browser::key calls at 25 Hz, not OS input.
The temporary catalog is copied using SQLite backup; source media are read only.
"""

import argparse
import json
import os
import signal
import sqlite3
import subprocess
import time
from pathlib import Path
from run import memory_tree


def metrics(path):
    events = [line.split() for line in path.read_text().splitlines()]
    keys = [e for e in events if e[0] == "key"]
    frames = [e for e in events if e[0] == "frame"]
    result = []
    for i, key in enumerate(keys):
        deadline = min(
            int(key[1]) + 40_000_000,
            int(keys[i + 1][1]) if i + 1 < len(keys) else 2**63,
        )
        matching = [
            f
            for f in frames
            if int(key[1]) <= int(f[1]) < deadline
            and f[2] == key[2]
            and f[4] == key[4]
            and (len(key) < 6 or (len(f) > 6 and key[5] == f[6]))
        ]
        ready = next((f for f in matching if f[5] == "1"), None)
        result.append(
            {
                "sequence": int(key[2]),
                "phase": int(key[3]),
                "cursor": int(key[4]),
                "identity": key[5] if len(key) > 5 else None,
                "input_ns": int(key[1]),
                "presented": bool(matching),
                "advanced": i == 0 or key[4] != keys[i - 1][4],
                "preview_ready": ready is not None,
                "latency_ms": (int(ready[1]) - int(key[1])) / 1e6 if ready else None,
            }
        )
    phases = []
    for phase in (1, 3, 5):
        rows = [r for r in result if r["phase"] == phase][1:]  # omit Home
        lat = sorted(r["latency_ms"] for r in rows if r["latency_ms"] is not None)
        intervals = sorted(
            (rows[j]["input_ns"] - rows[j - 1]["input_ns"]) / 1e6
            for j in range(1, len(rows))
        )
        phases.append(
            {
                "phase": phase,
                "down_inputs": len(rows),
                "selection_presented": sum(r["presented"] for r in rows),
                "preview_ready_before_next_input": sum(
                    r["preview_ready"] for r in rows
                ),
                "preview_not_ready_before_next_input": sum(
                    not r["preview_ready"] for r in rows
                ),
                "last_cursor": rows[-1]["cursor"] if rows else None,
                "unadvanced_selections": sum(not r["advanced"] for r in rows),
                "unique_selected_indices": len(set(r["cursor"] for r in rows)),
                "input_duration_s": (
                    (rows[-1]["input_ns"] - rows[0]["input_ns"]) / 1e9 if rows else None
                ),
                "input_interval_ms_p50": (
                    intervals[len(intervals) // 2] if intervals else None
                ),
                "input_interval_ms_p95": (
                    intervals[min(len(intervals) - 1, int(len(intervals) * 0.95))]
                    if intervals
                    else None
                ),
                "input_interval_ms_max": max(intervals) if intervals else None,
                "latency_ms_p50": lat[len(lat) // 2] if lat else None,
                "latency_ms_p95": (
                    lat[min(len(lat) - 1, int(len(lat) * 0.95))] if lat else None
                ),
            }
        )
    draws = [e for e in events if e[0] == "draw"]
    return {
        "phases": phases,
        "transitions": result,
        "columns": sorted(set(int(e[6]) for e in draws)),
        "max_loaded": max((int(e[7]) for e in draws), default=0),
        "done": any(e[0] == "done" for e in events),
    }


def trace_completed(path):
    """The explicit final marker distinguishes orderly app exit from a lost PID."""
    try:
        with path.open("rb") as stream:
            stream.seek(0, 2)
            stream.seek(max(0, stream.tell() - 256))
            tail = stream.read()
    except FileNotFoundError:
        return False
    lines = tail.splitlines()
    fields = lines[-1].split() if lines and tail.endswith(b"\n") else []
    return len(fields) == 2 and fields[0] == b"done" and fields[1].isdigit()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--runtime", type=Path, required=True)
    p.add_argument("--catalog", type=Path, required=True)
    p.add_argument("--executable", type=Path, required=True)
    p.add_argument("--out", type=Path, required=True)
    p.add_argument("--steps", type=int, default=600)
    p.add_argument("--smoke", action="store_true")
    p.add_argument("--only-background", action="store_true")
    p.add_argument("--all-loaded-pair", action="store_true")
    p.add_argument("--preload-all", action="store_true")
    p.add_argument("--heaptrack", type=Path)
    p.add_argument(
        "--incomplete-catalog",
        type=Path,
        help="Optional background-work comparison snapshot",
    )
    a = p.parse_args()
    runtime = a.runtime.resolve()
    if runtime == Path(os.environ.get("XDG_RUNTIME_DIR", "/nonexistent")):
        raise RuntimeError("Refusing desktop runtime")
    a.out.mkdir(parents=True, exist_ok=False)
    (a.out / "method.json").write_text(
        json.dumps(
            {
                "input": "Browser::key invoked on UI event loop, nominal25Hz; actual intervals reported",
                "preview": "Current cursor identity and dispatch sequence matched to drawn preview texture and Wayland frame callback; deadline=min(next input,40ms)",
                "presentation": "Headless compositor callback, not physical display presentation",
                "cache": "Fresh XDG cache per case; three repeated input traversals within one process",
                "geometry": "Sidebar hidden; inspector300px; browser content width constrained inside1280x960 hardware window",
                "idle": "10s between traversals;35s final idle then one malloc_trim(0),3s further idle",
                "memory": "1Hz parent/helper smaps_rollup;1Hz mallinfo2 main-thread samples; cache accounting from app logs",
                "source": "Read-only media; catalog copied with SQLite backup; background paused except explicitly named case",
            },
            indent=2,
        )
        + "\n"
    )
    base = {
        k: v
        for k, v in os.environ.items()
        if k
        not in (
            "DISPLAY",
            "DBUS_SESSION_BUS_ADDRESS",
            "UMBRIEL_SOCKET",
            "XDG_ACTIVATION_TOKEN",
            "DESKTOP_STARTUP_ID",
            "RUMMAGE_STARTUP_QUIT",
            "LIBGL_ALWAYS_SOFTWARE",
        )
    }
    base.update(
        XDG_RUNTIME_DIR=str(runtime),
        WAYLAND_DISPLAY="wayland-0",
        UMBRIEL_SOCKET=str(runtime / "umbriel-wayland-0.sock"),
        DBUS_SYSTEM_BUS_ADDRESS="unix:path=" + str(runtime / "no-system-bus"),
    )
    outputs = json.loads(
        subprocess.check_output(["umbriel", "outputs", "--json"], env=base, text=True)
    )
    if len(outputs) != 1 or outputs[0]["name"] != "BENCH":
        raise RuntimeError("Expected private BENCH output")
    (a.out / "outputs.json").write_text(json.dumps(outputs, indent=2))
    cases = [
        ("small-two", 2, 650),
        ("small-four", 2, 970),
        ("large-two", 4, 850),
        ("large-three", 4, 1150),
    ]
    if a.smoke:
        cases = cases[:1]
    if a.heaptrack:
        cases = cases[-1:]
    if a.incomplete_catalog and not a.smoke and not a.heaptrack:
        cases.append(("large-three-background", 4, 1150))
    if a.only_background:
        if not a.incomplete_catalog:
            p.error("--only-background needs --incomplete-catalog")
        cases = [("large-three-background", 4, 1150)]
    if a.all_loaded_pair:
        if not a.incomplete_catalog:
            p.error("--all-loaded-pair needs --incomplete-catalog")
        cases = [
            ("large-three-all-loaded", 4, 1150),
            ("large-three-background-all-loaded", 4, 1150),
        ]
        a.preload_all = True
    results = []
    for name, icon, width in cases:
        out = a.out / name
        out.mkdir()
        env = base.copy()
        for kind in ("CONFIG", "CACHE", "STATE", "DATA"):
            directory = out / kind.lower()
            directory.mkdir()
            env["XDG_" + kind + "_HOME"] = str(directory)
        catalog = out / "data/rummage/photos/catalog.sqlite3"
        catalog.parent.mkdir(parents=True)
        background = "-background" in name
        source_catalog = a.incomplete_catalog if background else a.catalog
        with sqlite3.connect(
            "file:" + str(source_catalog) + "?mode=ro", uri=True
        ) as src, sqlite3.connect(catalog) as dst:
            src.backup(dst)
            dst.execute(
                "update background_settings set paused=?", (0 if background else 1,)
            )
            dst.commit()

        def jobs():
            with sqlite3.connect(
                "file:" + str(catalog) + "?mode=ro", uri=True
            ) as connection:
                connection.row_factory = sqlite3.Row
                return [
                    dict(row)
                    for row in connection.execute(
                        "select kind,state,completed,total from jobs"
                    )
                ]

        def counts():
            with sqlite3.connect(
                "file:" + str(catalog) + "?mode=ro", uri=True
            ) as connection:
                row = connection.execute(
                    "select count(*),sum(metadata_signature != ''),sum(width>0 and height>0) from photos"
                ).fetchone()
                return dict(
                    zip(("photos", "metadata_attempted", "valid_dimensions"), row)
                )

        jobs_before = jobs()
        counts_before = counts()
        env["RUMMAGE_STARTUP_NS"] = str(time.monotonic_ns())
        env.update(
            PHOTO_TRACE=str(out / "trace.txt"),
            PHOTO_ICON=str(icon),
            PHOTO_WIDTH=str(width),
            PHOTO_STEPS=str(a.steps),
            ALLOCATOR_TRACE=str(out / "allocator.jsonl"),
            ALLOCATOR_CONTROL=str(out / "control"),
        )
        if a.preload_all:
            env["PHOTO_PRELOAD_ALL"] = "1"
        config = out / "config.toml"
        config.write_text('[icon]\ntheme="builtin"\nfollow_accent=false\n')
        empty = out / "empty"
        empty.mkdir()
        args = [str(a.executable), "--config", str(config), str(empty)]
        if a.heaptrack:
            args = [
                str(a.heaptrack),
                "--record-only",
                "-o",
                str(out / "heaptrack"),
                *args,
            ]
        with (out / "application.log").open("w") as log, (out / "memory.jsonl").open(
            "w"
        ) as mem:
            start = time.monotonic()
            proc = subprocess.Popen(
                args, env=env, stdout=log, stderr=log, start_new_session=True
            )
            print("started", name, proc.pid, flush=True)
            try:
                while proc.poll() is None:
                    if time.monotonic() - start > 360:
                        raise TimeoutError(name)
                    try:
                        target = proc.pid
                        allocator = out / "allocator.jsonl"
                        if a.heaptrack and allocator.exists():
                            lines = allocator.read_text().splitlines()
                            if lines:
                                target = json.loads(lines[0])["pid"]
                        sample = {
                            "elapsed_s": time.monotonic() - start,
                            "profile_wrapper_sample": bool(
                                a.heaptrack and target == proc.pid
                            ),
                            **memory_tree(target),
                        }
                    except (ProcessLookupError, FileNotFoundError, RuntimeError):
                        if proc.poll() is not None:
                            break
                        if a.heaptrack and trace_completed(out / "trace.txt"):
                            # The profiled process exits before heaptrack finishes writing
                            # its final compressed trace. Do not sample that vanished PID.
                            proc.wait(timeout=15)
                            break
                        raise
                    mem.write(json.dumps(sample) + "\n")
                    mem.flush()
                    time.sleep(1)
            finally:
                if proc.poll() is None:
                    os.killpg(proc.pid, signal.SIGTERM)
                    try:
                        proc.wait(10)
                    except subprocess.TimeoutExpired:
                        os.killpg(proc.pid, signal.SIGKILL)
                        proc.wait()
        result = {
            "name": name,
            "icon_index": icon,
            "content_width": width,
            "exit_code": proc.returncode,
            "elapsed_s": time.monotonic() - start,
            "profiled": bool(a.heaptrack),
            "background_work": background,
            "preload_all": a.preload_all,
            "jobs_before": jobs_before,
            "jobs_after": jobs(),
            "catalog_before": counts_before,
            "catalog_after": counts(),
            "metrics": metrics(out / "trace.txt"),
        }
        results.append(result)
        (a.out / "results.json").write_text(json.dumps(results, indent=2))
        print(
            "finished",
            name,
            result["elapsed_s"],
            result["metrics"]["columns"],
            result["metrics"]["phases"],
            flush=True,
        )
        if proc.returncode != 0 or not result["metrics"]["done"]:
            raise RuntimeError(name + " did not complete")


if __name__ == "__main__":
    main()
