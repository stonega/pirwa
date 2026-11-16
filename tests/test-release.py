#!/usr/bin/env python3
"""Publishing guards, with no package tools, network or credentials."""

import argparse
import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parent.parent
SPEC = importlib.util.spec_from_file_location("release", ROOT / "scripts/release.py")
release = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(release)


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="pirwa-release-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / "source"
        self.source.mkdir()
        archive = self.source / "pirwa-0.1.0.tar.xz"
        archive.write_bytes(b"source fixture")
        self.record = {"version": "0.1.0", "commit": "a" * 40, "dirty": False,
                       "epoch": 1, "app_id": release.APP_ID, "archive": archive.name,
                       "sha256": release.sha(archive)}
        release.write_json(self.source / "source.json", self.record)
        self.assets = self.root / "assets"
        for kind, names in {
            "deb": ["pirwa_0.1.0-1_amd64.deb"],
            "rpm": ["pirwa-0.1.0-1.fc45.x86_64.rpm", "pirwa-0.1.0-1.fc45.src.rpm"],
            "flatpak": [release.APP_ID + "-0.1.0-x86_64.flatpak"],
        }.items():
            directory = self.assets / kind
            directory.mkdir(parents=True)
            for name in names:
                (directory / name).write_bytes(name.encode())
            release.provenance(directory, self.record, kind)
        self.output = self.root / "release"
        self.args = argparse.Namespace(input=self.assets, source=self.source / "source.json",
                                       output=self.output)

    def test_complete_release_and_copr_command(self):
        release.manifest(self.args)
        release.verify(self.output, publish=True)
        with patch.object(release, "run") as run:
            release.copr(argparse.Namespace(input=self.output, project="stonegate/pirwa", dry_run=False))
            self.assertEqual(run.call_count, 2)
            self.assertEqual(run.call_args.args[0], ["copr-cli", "build", "--chroot",
                             "fedora-45-x86_64", "stonegate/pirwa",
                             str(self.output / "pirwa-0.1.0-1.fc45.src.rpm")])

    def test_missing_format(self):
        (self.assets / "flatpak/flatpak-provenance.json").unlink()
        with self.assertRaisesRegex(ValueError, "Exactly one"):
            release.manifest(self.args)

    def test_mixed_source(self):
        path = self.assets / "deb/deb-provenance.json"
        record = json.loads(path.read_text())
        record["source"]["commit"] = "b" * 40
        release.write_json(path, record)
        with self.assertRaisesRegex(ValueError, "different source"):
            release.manifest(self.args)

    def test_modified_asset(self):
        (self.assets / "deb/pirwa_0.1.0-1_amd64.deb").write_bytes(b"tampered")
        with self.assertRaisesRegex(ValueError, "modified release asset"):
            release.manifest(self.args)

    def test_symlink_asset(self):
        path = self.assets / "deb/pirwa_0.1.0-1_amd64.deb"
        target = self.root / "outside"
        path.rename(target)
        path.symlink_to(target)
        with self.assertRaisesRegex(ValueError, "modified release asset"):
            release.manifest(self.args)

    def test_dirty_source_cannot_publish(self):
        release.manifest(self.args)
        path = self.output / "release.json"
        record = json.loads(path.read_text())
        record["source"]["dirty"] = True
        release.write_json(path, record)
        with patch.object(release, "run") as run:
            with self.assertRaisesRegex(ValueError, "clean stable"):
                release.copr(argparse.Namespace(input=self.output, project="stonegate/pirwa", dry_run=False))
            run.assert_not_called()

    def test_development_source_cannot_publish(self):
        release.manifest(self.args)
        path = self.output / "release.json"
        record = json.loads(path.read_text())
        record["source"]["version"] = "0.1.0-dev"
        record["source"]["archive"] = "pirwa-0.1.0-dev.tar.xz"
        release.write_json(path, record)
        with self.assertRaisesRegex(ValueError, "clean stable"):
            release.verify(self.output, publish=True)

    def test_checksum_list_and_unlisted_files(self):
        release.manifest(self.args)
        sums = self.output / "SHA256SUMS"
        original = sums.read_text()
        sums.write_text("")
        with self.assertRaisesRegex(ValueError, "SHA256SUMS"):
            release.verify(self.output)
        sums.write_text(original)
        (self.output / "stale.rpm").write_bytes(b"old")
        with self.assertRaisesRegex(ValueError, "Unexpected files"):
            release.verify(self.output)

    def test_source_checksum(self):
        (self.source / self.record["archive"]).write_bytes(b"different source")
        with self.assertRaisesRegex(ValueError, "Source archive checksum"):
            release.inputs(self.source / "source.json")

    def test_asset_path_escape(self):
        path = self.assets / "deb/deb-provenance.json"
        record = json.loads(path.read_text())
        record["assets"] = {"../escape.deb": "0" * 64}
        release.write_json(path, record)
        with self.assertRaisesRegex(ValueError, "Invalid release asset filename"):
            release.manifest(self.args)

    def test_output_cannot_mix_old_files(self):
        self.output.mkdir()
        (self.output / "old.deb").touch()
        with self.assertRaisesRegex(ValueError, "empty output"):
            release.manifest(self.args)

    def test_srpm_keeps_stable_source_version(self):
        spec = release.rpm_spec((ROOT / "packaging/pirwa.spec").read_text(), "0.1.0")
        self.assertIn("%global pirwa_source_version 0.1.0\n", spec)
        self.assertNotIn("!?pirwa_source_version", spec)
        self.assertIn("Release:        1%{?dist}\n", spec)

    def test_tag_and_dirty_source_guards(self):
        repository = self.root / "git"
        repository.mkdir()
        subprocess.run(["git", "init", "-q", str(repository)], check=True)
        (repository / "meson.build").write_text("project('pirwa', 'c',\n version: '0.1.0')\n")
        subprocess.run(["git", "-C", str(repository), "add", "."], check=True)
        subprocess.run(["git", "-C", str(repository), "-c", "user.name=Test",
                        "-c", "user.email=test@example.invalid", "commit", "-qm", "fixture"], check=True)
        args = argparse.Namespace(root=repository, allow_dirty=False, tag="v0.2.0", output=self.output)
        with self.assertRaisesRegex(ValueError, "tag must match"):
            release.source(args)
        args.tag = None
        (repository / "meson.build").write_text("project('pirwa', 'c',\n version: '0.1.0')\n# dirty\n")
        with self.assertRaisesRegex(ValueError, "Commit the candidate"):
            release.source(args)


if __name__ == "__main__":
    unittest.main()
