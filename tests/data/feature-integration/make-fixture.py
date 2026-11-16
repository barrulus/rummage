"""Regenerate the deterministic archive used by feature_integration_test.cpp."""
from pathlib import Path
from zipfile import ZipFile, ZipInfo, ZIP_DEFLATED

with ZipFile(Path(__file__).with_name("notes.zip"), "w") as archive:
    for name, heading in (("a.md", "First"), ("b.md", "Second")):
        info = ZipInfo(name, (2026, 11, 16, 12, 0, 0))
        info.create_system = 3
        info.external_attr = 0o100644 << 16
        archive.writestr(info, f"# {heading} note\n\nSome **bold** and `code`.\n", ZIP_DEFLATED)
