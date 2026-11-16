#!/usr/bin/env python3
"""Compare the translations in data/i18n/*.toml with the texts the code translates.

For each catalog it lists the texts the code passes to tr()/trf() that the catalog lacks
(they show in English) and the catalog entries no code uses any more. With --unwrapped it
also lists string literals in interface code that look user-visible but are not passed to
tr()/trf(): candidates to wrap, judged by a heuristic, so read before acting.

Usage: scripts/i18n-check.py [--unwrapped] [--strict] [LANGUAGE...]
--strict exits 1 when a catalog misses texts or keeps unused ones. Needs Python 3.11+.
"""

import argparse
import re
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
LITERAL = r'"(?:[^"\\\n]|\\.)*"'
ADJACENT = re.compile(rf"(?:{LITERAL}\s*){{2,}}")
CALL = re.compile(rf"\b(?:i18n::)?trf?\(\s*({LITERAL})")
# Interface code: views, the app, and the platform/fs messages the views show.
INTERFACE = ["src/views", "src/app", "src/platform/src", "src/fs/src", "src/ui/src"]


def unescape(literal):
    """The text of a C++ string literal: \\n, \\t, \\uXXXX and quoted characters."""
    body = literal[1:-1]

    def one(match):
        if match[1]:
            return chr(int(match[1], 16))
        return {"n": "\n", "t": "\t"}.get(match[2], match[2])

    return re.sub(r"\\(?:u([0-9a-fA-F]{4})|(.))", one, body)


def join_adjacent(source):
    """"a" "b" -> "ab", as the compiler joins them."""

    def join(match):
        parts = re.findall(LITERAL, match[0])
        return '"' + "".join(p[1:-1] for p in parts) + '"'

    return ADJACENT.sub(join, source)


def sources():
    for path in sorted((ROOT / "src").rglob("*")):
        if path.suffix in (".cpp", ".hpp") and path.is_file():
            yield path


def used_texts():
    """Texts passed to tr()/trf(), and every literal of the code (for unused entries)."""
    used, literals = set(), set()
    for path in sources():
        text = join_adjacent(path.read_text(encoding="utf-8"))
        used.update(unescape(m) for m in CALL.findall(text))
        literals.update(unescape(m) for m in re.findall(LITERAL, text))
    return used, literals


def catalogs(wanted):
    for path in sorted((ROOT / "data" / "i18n").glob("*.toml")):
        if wanted and path.stem not in wanted:
            continue
        with path.open("rb") as file:
            yield path.stem, tomllib.load(file)


# A literal looks user-visible when it holds a word with a capital or two words, and is not an
# identifier, path, MIME type, format-only string, log or config key.
SKIP_LINE = re.compile(
    r"log::|#include|throw |std::getenv|\.id\s*=|LOG|static_assert|REQUIRE|CHECK|"
    r"\bkey\b\s*==|\bid\s*==|\.starts_with|\.ends_with|find\(|== \"|!= \""
)


def looks_visible(text):
    if len(text) < 3 or not re.search(r"[A-Za-z]{2}", text):
        return False
    if re.fullmatch(r"[a-z0-9_.:/\-]+", text):  # identifiers, keys, paths
        return False
    if re.search(r"^[a-z]+/[a-z0-9.+\-]+$|\.(desktop|toml|png|svg|so)\b|^--|^\w+://|^/", text):
        return False
    return bool(re.search(r"[A-Z][a-z]|[a-z]{2,} [a-z]{2,}", text))


RUN = re.compile(rf"{LITERAL}(?:\s*{LITERAL})*")
COMMENT = re.compile(r"//[^\n]*")


def inside_tr(code, start):
    """Whether position start is inside the parentheses of a tr(/trf( call not yet closed."""
    window = code[max(0, start - 400):start]
    for match in reversed(list(re.finditer(r"\btrf?\(", window))):
        between = re.sub(LITERAL, '""', window[match.end():])
        if between.count("(") >= between.count(")"):
            return True
    return False


def unwrapped(entries):
    for folder in INTERFACE:
        for path in sorted((ROOT / folder).rglob("*.cpp")):
            # Comments become blanks of the same length, so offsets and lines stay true.
            code = COMMENT.sub(lambda m: " " * len(m[0]), path.read_text(encoding="utf-8"))
            for run in RUN.finditer(code):
                line_start = code.rfind("\n", 0, run.start()) + 1
                line_end = code.find("\n", run.start())
                line = code[line_start:line_end if line_end != -1 else len(code)]
                if SKIP_LINE.search(line) or inside_tr(code, run.start()):
                    continue
                text = "".join(unescape(part) for part in re.findall(LITERAL, run[0]))
                if looks_visible(text) and text not in entries:
                    number = code.count("\n", 0, run.start()) + 1
                    yield f"{path.relative_to(ROOT)}:{number}: {text}"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("languages", nargs="*", help="language codes (default: all)")
    parser.add_argument("--unwrapped", action="store_true",
                        help="also list literals that look visible but are not translated")
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 when a catalog misses texts or keeps unused entries")
    args = parser.parse_args()

    used, literals = used_texts()
    failed = False
    found = False
    for code, catalog in catalogs(set(args.languages)):
        found = True
        texts = catalog.get("texts", {})
        missing = sorted(t for t in used if t not in texts and t.strip(" ({}),.:;") != "")
        unused = sorted(t for t in texts if t not in literals)
        name = catalog.get("language", {}).get("name", code)
        print(f"{code} ({name}): {len(texts)} texts, {len(missing)} missing, {len(unused)} unused")
        for text in missing:
            print(f"  missing: {text!r}")
        for text in unused:
            print(f"  unused:  {text!r}")
        failed |= bool(missing or unused)
        if args.unwrapped:
            for line in unwrapped(texts):
                print(f"  not wrapped? {line}")
    if not found:
        print("no catalog in data/i18n matches", file=sys.stderr)
        return 2
    return 1 if args.strict and failed else 0


if __name__ == "__main__":
    sys.exit(main())
