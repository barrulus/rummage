#!/usr/bin/env python3
"""Check a generated site for broken internal links, assets, and search entries.

Usage: python3 website/check.py website/public
Requires only Python 3.11+. Does not contact external sites.
"""

from collections import Counter
from html.parser import HTMLParser
import json
from pathlib import Path
import sys
import tomllib
from urllib.parse import unquote, urljoin, urlsplit


class Page(HTMLParser):
    def __init__(self, source):
        super().__init__()
        self.ids = []
        self.links = []
        self.headings = 0
        self.feed(source)

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        if "id" in attrs:
            self.ids.append(attrs["id"])
        if tag == "h1":
            self.headings += 1
        for key in ("href", "src", "poster", "data-search-index"):
            if attrs.get(key):
                self.links.append(attrs[key])


def navigation_errors(navigation, docs):
    """Every canonical guide must be published and searchable exactly once."""
    slugs = [item["slug"] for group in navigation["groups"] for item in group["items"]]
    guides = {path.stem for path in docs.glob("*.md") if path.name != "README.md"}
    errors = []
    for slug in sorted(guides - set(slugs)):
        errors.append(f"Canonical guide missing from website navigation: {slug}")
    for slug in sorted(set(slugs) - guides):
        errors.append(f"Website navigation has no canonical guide: {slug}")
    for slug, count in Counter(slugs).items():
        if count > 1:
            errors.append(f"Duplicate website navigation guide: {slug}")
    return errors


def main():
    root = Path(sys.argv[1] if len(sys.argv) > 1 else "website/public").resolve()
    config = tomllib.loads(Path(__file__).with_name("hugo.toml").read_text())
    host = urlsplit(config["baseURL"]).netloc
    pages = {path: Page(path.read_text()) for path in root.rglob("*.html")}
    errors = []
    checked = 0
    if not pages:
        raise SystemExit(f"No built pages found in {root}")

    def check_link(source, target):
        nonlocal checked
        url = urlsplit(target)
        if (url.scheme and url.scheme not in ("http", "https")) or (url.netloc and url.netloc != host):
            return
        base = "/" + str(source.relative_to(root))
        resolved = urlsplit(urljoin(base, target))
        destination = root / unquote(resolved.path).lstrip("/")
        if destination.is_dir():
            destination /= "index.html"
        checked += 1
        if not destination.is_file():
            errors.append(f"{source.relative_to(root)}: missing target {target}")
        elif resolved.fragment and destination in pages:
            if unquote(resolved.fragment) not in pages[destination].ids:
                errors.append(f"{source.relative_to(root)}: missing anchor {target}")

    for source, page in pages.items():
        if page.headings != 1:
            errors.append(f"{source.relative_to(root)}: expected one h1, got {page.headings}")
        for anchor, count in Counter(page.ids).items():
            if count > 1:
                errors.append(f"{source.relative_to(root)}: duplicate id {anchor}")
        for target in page.links:
            check_link(source, target)

    navigation = tomllib.loads(Path(__file__).with_name("data").joinpath("navigation.toml").read_text())
    errors.extend(navigation_errors(navigation, Path(__file__).resolve().parents[1] / "docs"))
    expected = {f'/docs/{item["slug"]}/' for group in navigation["groups"] for item in group["items"]}
    index = json.loads((root / "index.json").read_text())
    if {page["url"] for page in index} != expected:
        errors.append("Search index must include every guide in the navigation exactly once")
    if len(index) != len(expected):
        errors.append("Search index has duplicate or missing guides")
    for page in index:
        check_link(root / "index.html", page["url"])
        if not page["title"] or not page["text"].strip():
            errors.append(f"Empty search entry: {page['url']}")
    downloads = pages.get(root / "downloads/index.html")
    if downloads is None:
        errors.append("Missing download page")
    else:
        packages = [url for url in downloads.links if "/releases/download/development/" in url]
        if len(set(packages)) != 5:
            errors.append("Download page must link to all five distribution packages")
    for required in ("CNAME", ".nojekyll", "docs/config.example.toml", "404.html"):
        if not (root / required).is_file():
            errors.append(f"Missing deployment asset: {required}")
    if (root / "CNAME").read_text().strip() != host:
        errors.append("CNAME does not match the configured domain")
    if errors:
        print("\n".join(errors), file=sys.stderr)
        return 1
    print(f"Site checks passed: {len(pages)} HTML pages, {checked} internal links/assets, "
          f"{len(index)} searchable guides, and five distribution downloads.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
