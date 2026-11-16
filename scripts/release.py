#!/usr/bin/env python3
"""Build release assets from one source snapshot and submit its SRPM with copr-cli."""

import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import tarfile
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
APP_ID = "io.github.stonega.Pirwa"
VERSION = re.compile(r"[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9.]+)?")
ASSET = re.compile(r"[A-Za-z0-9][A-Za-z0-9._+~-]*")


def run(command, **kwargs):
    print("+ " + " ".join(map(str, command)), flush=True)
    return subprocess.run(list(map(str, command)), check=True, **kwargs)


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + "\n")


def empty_output(path):
    path.mkdir(parents=True, exist_ok=True)
    if any(path.iterdir()):
        raise ValueError("Use an empty output directory: " + str(path))


def version(root):
    match = re.search(r"^\s*version:\s*'([^']+)'", (root / "meson.build").read_text(), re.MULTILINE)
    if not match or not VERSION.fullmatch(match[1]):
        raise ValueError("Invalid Meson release version")
    return match[1]


def source(args):
    root = args.root.resolve()
    ver = version(root)
    commit = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    dirty = bool(subprocess.check_output(["git", "status", "--porcelain"], cwd=root, text=True))
    if dirty and not args.allow_dirty:
        raise ValueError("Commit the candidate before building release assets; --allow-dirty is for local QA")
    if args.tag and (args.tag != "v" + ver or dirty):
        raise ValueError("The tag must match the clean Meson version")
    if args.tag:
        tagged = subprocess.check_output(["git", "rev-parse", args.tag + "^{commit}"],
                                         cwd=root, text=True).strip()
        if tagged != commit:
            raise ValueError("The release tag does not point at this source commit")
    run(["python3", root / "scripts/check-release-metadata.py",
         "--source-root", root, "--app-id", APP_ID, "--version", ver])
    empty_output(args.output)
    archive = args.output / ("pirwa-" + ver + ".tar.xz")
    epoch = int(subprocess.check_output(["git", "show", "-s", "--format=%ct", "HEAD"], cwd=root, text=True))
    if dirty:
        names = subprocess.check_output(
            ["git", "ls-files", "-z", "--cached", "--others", "--exclude-standard"], cwd=root
        ).decode().split("\0")
        with tarfile.open(archive, "w:xz") as tar:
            for name in sorted(set(names) - {""}):
                path = root / name
                if path.is_file() or path.is_symlink():
                    tar.add(path, arcname="pirwa-" + ver + "/" + name, recursive=False)
    else:
        with archive.open("wb") as output:
            producer = subprocess.Popen(["git", "archive", "--format=tar", "--prefix=pirwa-" + ver + "/",
                                         commit], cwd=root, stdout=subprocess.PIPE)
            try:
                run(["xz", "-T2"], stdin=producer.stdout, stdout=output)
            finally:
                producer.stdout.close()
                if producer.wait():
                    raise RuntimeError("git archive failed")
    write_json(args.output / "source.json", {
        "version": ver, "commit": commit, "dirty": dirty, "epoch": epoch, "app_id": APP_ID,
        "archive": archive.name, "sha256": sha(archive),
    })


def validate_source(record):
    if (not VERSION.fullmatch(record["version"])
            or not re.fullmatch(r"(?:[0-9a-f]{40}|[0-9a-f]{64})", record["commit"])
            or record["app_id"] != APP_ID or type(record["dirty"]) is not bool
            or type(record["epoch"]) is not int or record["epoch"] < 0
            or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
        raise ValueError("Invalid source provenance")
    name = record["archive"]
    if Path(name).name != name or name != "pirwa-" + record["version"] + ".tar.xz":
        raise ValueError("Invalid source archive filename")


def inputs(path):
    record = json.loads(path.read_text())
    validate_source(record)
    name = record["archive"]
    archive = path.parent / name
    if archive.is_symlink() or not archive.is_file() or sha(archive) != record["sha256"]:
        raise ValueError("Source archive checksum mismatch")
    return record, archive


def unpack(archive, destination, ver):
    with tarfile.open(archive) as tar:
        prefix = "pirwa-" + ver
        if any(Path(m.name).parts[0] != prefix for m in tar.getmembers()):
            raise ValueError("Source archive prefix differs from the version")
        tar.extractall(destination, filter="data")
    return destination / prefix


def rpm_spec(spec, ver):
    # Definitions must survive SRPM extraction on COPR without CLI overrides.
    spec, count = re.subn(r"^%\{!\?pirwa_source_version:.*$", "%global pirwa_source_version " + ver,
                         spec, flags=re.MULTILINE)
    if count != 1:
        raise ValueError("Missing RPM source-version definition")
    release = "1" if "-" not in ver else "0.1." + ver.split("-", 1)[1]
    return re.sub(r"^Release:.*$", "Release:        " + release + "%{?dist}", spec,
                  flags=re.MULTILINE)


def rpm(args):
    record, archive = inputs(args.source)
    ver = record["version"]
    empty_output(args.output)
    with tempfile.TemporaryDirectory(prefix="pirwa-rpm-") as temporary:
        top = Path(temporary)
        root = unpack(archive, top / "source", ver)
        (top / "SOURCES").mkdir()
        (top / "SPECS").mkdir()
        shutil.copy2(archive, top / "SOURCES" / archive.name)
        spec = (root / "packaging/pirwa.spec").read_text()
        spec_path = top / "SPECS/pirwa.spec"
        spec_path.write_text(rpm_spec(spec, ver))
        environment = dict(os.environ, SOURCE_DATE_EPOCH=str(record["epoch"]))
        run(["rpmbuild", "--define", "_topdir " + str(top), "--define", "_smp_mflags -j2",
             "--define", "_smp_build_ncpus 2", "-ba", spec_path], env=environment)
        for folder in ["RPMS", "SRPMS"]:
            for package in (top / folder).rglob("*.rpm"):
                shutil.copy2(package, args.output / package.name)
    provenance(args.output, record, "rpm")


def deb(args):
    record, archive = inputs(args.source)
    empty_output(args.output)
    with tempfile.TemporaryDirectory(prefix="pirwa-deb-") as temporary:
        root = unpack(archive, Path(temporary), record["version"])
        shutil.copytree(root / "packaging/debian", root / "debian")
        (root / "debian/rules").chmod(0o755)
        ver = record["version"].replace("-", "~", 1) + "-1"
        date = datetime.datetime.fromtimestamp(record["epoch"], datetime.timezone.utc)
        date_text = date.strftime("%a, %d %b %Y %H:%M:%S +0000")
        (root / "debian/changelog").write_text(
            f"pirwa ({ver}) unstable; urgency=medium\n\n"
            "  * Package Pirwa " + record["version"] + ".\n\n"
            f" -- Pirwa Contributors <2262007+stonega@users.noreply.github.com>  {date_text}\n")
        environment = dict(os.environ, SOURCE_DATE_EPOCH=str(record["epoch"]),
                           DEB_BUILD_OPTIONS="parallel=2")
        run(["dpkg-buildpackage", "-us", "-uc", "-b"], cwd=root, env=environment)
        for pattern in ["*.deb", "*.ddeb", "*.buildinfo", "*.changes"]:
            for package in root.parent.glob(pattern):
                shutil.copy2(package, args.output / package.name)
    provenance(args.output, record, "deb")


def provenance(output, record, kind):
    write_json(output / (kind + "-provenance.json"), {
        "kind": kind, "source": record,
        "assets": {p.name: sha(p) for p in sorted(output.iterdir())
                   if p.is_file() and not p.name.endswith("-provenance.json")},
    })


def flatpak(args):
    record, archive = inputs(args.source)
    empty_output(args.output)
    work = args.work.resolve()
    work.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="pirwa-flatpak-source-") as temporary:
        root = unpack(archive, Path(temporary), record["version"])
        manifest = json.loads((root / "packaging/flatpak" / (APP_ID + ".json")).read_text())
        manifest["modules"][-1]["sources"] = [
            {"type": "archive", "path": str(archive.resolve()), "sha256": record["sha256"]}]
        # Local patches are covered by the source archive's checksum. Keep them
        # beside the rendered manifest after the temporary source is removed.
        for module in manifest["modules"]:
            for item in module["sources"]:
                if "path" in item and not Path(item["path"]).is_absolute():
                    local = root / "packaging/flatpak" / item["path"]
                    target = work / local.name
                    shutil.copy2(local, target)
                    item["path"] = str(target)
        path = work / (APP_ID + ".json")
        write_json(path, manifest)
    repo = work / "repo"
    run(["flatpak-builder", "--user", "--force-clean", "--disable-rofiles-fuse", "--jobs=2",
         "--repo=" + str(repo), "--default-branch=stable", work / "app", path])
    bundle = args.output / (APP_ID + "-" + record["version"] + "-x86_64.flatpak")
    run(["flatpak", "build-bundle", repo, bundle, APP_ID, "stable",
         "--runtime-repo=https://flathub.org/repo/flathub.flatpakrepo"])
    provenance(args.output, record, "flatpak")


def manifest(args):
    empty_output(args.output)
    records = [json.loads(p.read_text()) for p in sorted(args.input.rglob("*-provenance.json"))]
    if {r["kind"] for r in records} != {"deb", "rpm", "flatpak"} or len(records) != 3:
        raise ValueError("Exactly one DEB, RPM and Flatpak build is required")
    source = records[0]["source"]
    if any(r["source"] != source for r in records):
        raise ValueError("Release assets came from different source snapshots")
    assets = {}
    for record in records:
        for name, digest in record["assets"].items():
            if not ASSET.fullmatch(name):
                raise ValueError("Invalid release asset filename")
            matches = list(args.input.rglob(name))
            if len(matches) != 1 or matches[0].is_symlink() or sha(matches[0]) != digest or name in assets:
                raise ValueError("Missing, duplicate or modified release asset: " + name)
            assets[name] = digest
            shutil.copy2(matches[0], args.output / name)
    source_record, archive = inputs(args.source)
    if source_record != source:
        raise ValueError("Source archive differs from the binary builds")
    shutil.copy2(archive, args.output / archive.name)
    assets[archive.name] = sha(archive)
    write_json(args.output / "release.json", {"source": source, "assets": assets})
    (args.output / "SHA256SUMS").write_text(
        "".join(digest + "  " + name + "\n" for name, digest in sorted(assets.items())))
    verify(args.output)


def verify(directory, publish=False):
    record = json.loads((directory / "release.json").read_text())
    source = record["source"]
    validate_source(source)
    if publish and (source["dirty"] or not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", source["version"])):
        raise ValueError("Publishing requires a clean stable release snapshot")
    names = record["assets"]
    for name, digest in names.items():
        path = directory / name
        if (not ASSET.fullmatch(name) or not re.fullmatch(r"[0-9a-f]{64}", digest)
                or path.is_symlink() or not path.is_file() or sha(path) != digest):
            raise ValueError("Release asset checksum mismatch: " + name)
    ver = source["version"]
    deb_version = re.escape(ver.replace("-", "~", 1))
    if (len([n for n in names if re.fullmatch(r"pirwa_" + deb_version + r"-1_amd64\.deb", n)]) != 1
            or APP_ID + "-" + ver + "-x86_64.flatpak" not in names):
        raise ValueError("Missing DEB or Flatpak bundle")
    rpm_version = ver.split("-", 1)[0]
    rpm_release = "1" if "-" not in ver else "0.1." + ver.split("-", 1)[1]
    rpm_name = "pirwa-" + re.escape(rpm_version + "-" + rpm_release) + r"(?:\.[A-Za-z0-9]+)?"
    if len([n for n in names if re.fullmatch(rpm_name + r"\.x86_64\.rpm", n)]) != 1:
        raise ValueError("Missing binary RPM")
    if (len([n for n in names if n.endswith(".src.rpm")]) != 1
            or not any(re.fullmatch(rpm_name + r"\.src\.rpm", n) for n in names)):
        raise ValueError("Exactly one source RPM is required for COPR")
    if names.get(source["archive"]) != source["sha256"]:
        raise ValueError("Missing matching source archive")
    sums = "".join(digest + "  " + name + "\n" for name, digest in sorted(names.items()))
    if (directory / "SHA256SUMS").read_text() != sums:
        raise ValueError("SHA256SUMS differs from release.json")
    expected = set(names) | {"release.json", "SHA256SUMS"}
    if {p.name for p in directory.iterdir()} != expected:
        raise ValueError("Unexpected files in release directory")
    return record


def copr(args):
    record = verify(args.input, publish=not args.dry_run)
    if not re.fullmatch(r"[A-Za-z0-9_-]+/[A-Za-z0-9_.-]+", args.project):
        raise ValueError("Use an explicit COPR owner/project")
    srpm = next(args.input / name for name in record["assets"] if name.endswith(".src.rpm"))
    command = ["copr-cli", "build", "--chroot", "fedora-45-x86_64", args.project, str(srpm)]
    if args.dry_run:
        print(json.dumps(command))
    else:
        run(["copr-cli", "get", args.project])
        run(command)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    cmd = commands.add_parser("source")
    cmd.add_argument("--root", type=Path, default=ROOT)
    cmd.add_argument("--output", type=Path, required=True)
    cmd.add_argument("--tag")
    cmd.add_argument("--allow-dirty", action="store_true")
    for name in ["rpm", "deb", "flatpak"]:
        cmd = commands.add_parser(name)
        cmd.add_argument("--source", type=Path, required=True)
        cmd.add_argument("--output", type=Path, required=True)
        if name == "flatpak":
            cmd.add_argument("--work", type=Path, required=True)
    cmd = commands.add_parser("manifest")
    cmd.add_argument("--source", type=Path, required=True)
    cmd.add_argument("--input", type=Path, required=True)
    cmd.add_argument("--output", type=Path, required=True)
    cmd = commands.add_parser("verify")
    cmd.add_argument("--input", type=Path, required=True)
    cmd.add_argument("--publish", action="store_true")
    cmd = commands.add_parser("copr")
    cmd.add_argument("--input", type=Path, required=True)
    cmd.add_argument("--project", default="stonegate/pirwa")
    cmd.add_argument("--dry-run", action="store_true")
    args = parser.parse_args()
    try:
        if args.command == "verify":
            verify(args.input, publish=args.publish)
        else:
            globals()[args.command](args)
    except (KeyError, TypeError, ValueError, OSError, subprocess.CalledProcessError, tarfile.TarError) as error:
        parser.exit(1, str(error) + "\n")


if __name__ == "__main__":
    main()
