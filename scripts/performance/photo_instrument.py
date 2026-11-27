#!/usr/bin/env python3
"""Build private Photo Library benchmark objects; shipped source/binary unchanged."""

import argparse
import hashlib
import json
import shlex
import subprocess
from pathlib import Path

p = argparse.ArgumentParser()
p.add_argument("--source", type=Path, required=True)
p.add_argument("--build", type=Path, required=True)
p.add_argument("--out", type=Path, required=True)
a = p.parse_args()
a.source, a.build, a.out = map(Path.resolve, (a.source, a.build, a.out))
a.out.mkdir(parents=True, exist_ok=True)
header = Path(__file__).with_name("photo_probe.hpp").resolve()
commands = subprocess.check_output(
    ["ninja", "-C", str(a.build), "-t", "commands", "rummage"], text=True
).splitlines()
link = commands[-1]
provenance = {}
for relative, stem in [
    ("src/app/main.cpp", "main"),
    ("src/views/src/info_pane.cpp", "info_pane"),
]:
    original = (a.source / relative).read_text()
    code = "#include " + json.dumps(str(header)) + "\n" + original
    if stem == "main":
        patches = [
            (
                "columns->resume_photo_work();",
                "columns->resume_photo_work();\n                photo_probe::install(loop, *columns, window);",
            ),
            (
                "columns->draw(renderer, content, palette, icons,",
                "photo_probe::begin_draw();\n                        content.width = photo_probe::width();\n                        columns->draw(renderer, content, palette, icons,",
            ),
            (
                "note_state();  // every change shows in a frame, so frames are where to look",
                "photo_probe::draw(*columns);\n                    note_state();  // every change shows in a frame, so frames are where to look",
            ),
            (
                "window.set_frame_shown_handler([&] {",
                "window.set_frame_shown_handler([&] {\n                photo_probe::frame();",
            ),
        ]
    else:
        patches = [
            (
                "if (thumbnail != nullptr) {",
                "photo_probe::preview(thumbnail != nullptr);\n        if (thumbnail != nullptr) {",
            )
        ]
    for before, after in patches:
        if code.count(before) != 1:
            raise RuntimeError((stem, before, code.count(before)))
        code = code.replace(before, after)
    source = a.out / (stem + ".cpp")
    source.write_text(code)
    line = next(c for c in commands if " -c " in c and c.endswith("/" + relative))
    args = shlex.split(line)
    args.insert(1, "-I" + str((a.source / relative).parent))
    old = args[args.index("-o") + 1]
    obj = str(a.out / (stem + ".o"))
    args[args.index("-o") + 1] = obj
    args[args.index("-c") + 1] = str(source)
    if "-MF" in args:
        args[args.index("-MF") + 1] = obj + ".d"
    subprocess.run(args, cwd=a.build, check=True)
    provenance[stem] = {
        "source_sha256": hashlib.sha256(original.encode()).hexdigest(),
        "compile": args,
    }
    if stem == "main":
        link = link.replace(old, shlex.quote(obj))
    else:
        link = link.replace(" -o rummage ", " " + shlex.quote(obj) + " -o rummage ")
link = link.replace(" -o rummage ", " -o " + shlex.quote(str(a.out / "rummage")) + " ")
subprocess.run(link, cwd=a.build, shell=True, check=True)
for child in a.build.iterdir():
    if child.name.startswith("rummage-") or child.name in (
        "engines",
        "librummage-photos.so",
    ):
        dest = a.out / child.name
        if not dest.exists():
            dest.symlink_to(child.resolve())
provenance["link"] = link
provenance["probe_sha256"] = hashlib.sha256(header.read_bytes()).hexdigest()
provenance["allocator_probe_sha256"] = hashlib.sha256(
    header.with_name("allocator_probe.hpp").read_bytes()
).hexdigest()
provenance["original_binary_sha256"] = hashlib.sha256(
    (a.build / "rummage").read_bytes()
).hexdigest()
provenance["revision"] = subprocess.check_output(
    ["git", "-C", str(a.source), "rev-parse", "HEAD"], text=True
).strip()
provenance["diff"] = subprocess.check_output(
    ["git", "-C", str(a.source), "diff"], text=True
)
provenance["binary_sha256"] = hashlib.sha256(
    (a.out / "rummage").read_bytes()
).hexdigest()
(a.out / "instrumentation.json").write_text(json.dumps(provenance, indent=2) + "\n")
