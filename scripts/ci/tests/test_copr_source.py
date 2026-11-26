import importlib.util
import io
import json
from pathlib import Path
import tarfile
import tempfile
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "copr_source", Path(__file__).resolve().parents[1] / "copr-source.py")
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)


class CoprSourceTest(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.source = self.root / "input"
        self.source.mkdir()
        self.output = self.root / "output"
        with tarfile.open(self.source / "source.tar.gz", "w:gz") as archive:
            for name, content in {
                "packaging/rpm/rummage.spec": "Version: 0.6.1\nRelease: 1%{?dist}\n"
                                             "Source0:        rummage-source.tar.gz\n",
                "third_party/pdfium-sources/fixture": "source",
            }.items():
                info = tarfile.TarInfo("rummage-checkout/" + name)
                data = content.encode()
                info.size = len(data)
                archive.addfile(info, io.BytesIO(data))
        self.metadata = {"commit": "a" * 40, "version": "0.6.1", "timestamp": "20261126000000",
                         "sha256": module.digest(self.source / "source.tar.gz")}
        self.save()

    def save(self):
        (self.source / "source.json").write_text(json.dumps(self.metadata))

    def test_development_input_retains_exact_source_and_identifiable_release(self):
        identity = module.prepare(self.source, self.output, "development")
        self.assertEqual(module.digest(self.output / "SOURCES/rummage-source.tar.gz"), self.metadata["sha256"])
        self.assertEqual(identity["release"], "0.20261126000000.gitaaaaaaaaaaaa%{?dist}")
        self.assertIn(identity["release"], (self.output / "SPECS/rummage.spec").read_text())
        self.assertEqual(identity["spec_sha256"], module.digest(self.output / "SPECS/rummage.spec"))

    def test_corrupt_source_is_rejected_before_output_creation(self):
        self.metadata["sha256"] = "0" * 64
        self.save()
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            module.prepare(self.source, self.output, "development")
        self.assertFalse(self.output.exists())

    def test_stable_requires_matching_existing_release_tag(self):
        with self.assertRaisesRegex(ValueError, "require --tag"):
            module.prepare(self.source, self.output, "stable")
        with patch.object(module.subprocess, "check_output", return_value="b" * 40):
            with self.assertRaisesRegex(ValueError, "does not point"):
                module.prepare(self.source, self.output, "stable", "v0.6.1")
        with patch.object(module.subprocess, "check_output", return_value="a" * 40):
            result = module.prepare(self.source, self.output, "stable", "v0.6.1")
        self.assertEqual(result["release"], "1%{?dist}")
