# Release packages and publishing

Each release provides one standard `pirwa_<version>-1_amd64.deb`, Fedora 45
x86_64 RPM, source RPM, and `io.github.stonega.Pirwa-<version>-x86_64.flatpak`
on GitHub Releases. Debug packages, source tarball, `release.json` provenance,
and `SHA256SUMS` accompany them. The source RPM is also rebuilt and published
in `stonegate/pirwa` using **copr-cli**. GitHub's account is `stonega`; the
authenticated COPR account is `stonegate`.

On 2026-11-16 the empty COPR project and GitHub `release` environment were
created, and its `COPR_CONFIG` secret was configured. No development package
was submitted or release published during this setup.

The current patch release is [v0.1.2](v0.1.2.md); its
[publication record](v0.1.2-validation.md) lists the verified packages and checks.
The first release is v0.1.0;
see its [release notes](v0.1.0.md) for scope and known limitations.
The development version cannot be published by the helper.
For subsequent releases, review the [acceptance record](v0.1.0-readiness.md),
update Meson/AppStream/RPM versions and release date together, write
`docs/release/v<version>.md`, commit the candidate, then create its matching tag.

## Package compatibility

The DEB has no distribution name or variant. Debian 13 is the build baseline;
`dpkg-shlibdeps` records the actual GTK, GLib, libc and other shared-library
requirements. A compatible DEB-based system can install the same file with
`sudo apt install ./pirwa_<version>-1_amd64.deb`. A common DEB cannot bypass
library or package dependencies on older systems. The RPM targets Fedora 45.
Both install the host QEMU/libvirt packages and recommend virt-viewer, UEFI and
software TPM support. The application itself remains C17 and native GTK 4.
Git is a build-only requirement for the release guard tests.
Local GitHub RPMs are unsigned; COPR signs its own repository builds. Package
installation QA checks the local RPM digest without requiring a release signature.

The Flatpak uses GNOME Platform 51, pins bundled sources with SHA-256, and
ships libvirt client/test drivers, libvirt-glib, SPICE GLib (GTK disabled),
libosinfo, osinfo-db, libslirp and dconf. Install with
`flatpak install --user ./io.github.stonega.Pirwa-<version>-x86_64.flatpak`.
The bundle declares Flathub as its runtime source. No Flathub app submission
is required to install this GitHub asset.

Pyparsing is a pinned build-only dependency for SPICE's code generator and is
removed during cleanup. The vendored libosinfo patch is upstream commit
`0adf38535637ec668e658d43f04f60f11f51574f`, needed for the SDK's libxml2;
its bytes are covered by the Pirwa source archive checksum.

Flatpak hosts still need QEMU/KVM and active libvirt Session/System sockets,
plus virt-viewer for external consoles and host firmware/TPM packages for
guests that need them. `LIBVIRT_AUTOSTART=0` prevents the bundled client from
trying to launch a daemon inside the sandbox. Package installation cannot
replace host KVM support or system authorization. Real-host acceptance includes
Session discovery/create/start, System authorization, ISO and owned storage,
proxy helper startup, SPICE/audio, external viewer and Boxes import.

The Flatpak deliberately grants host filesystem access for libvirt storage and
ISOs, access to libvirt/Boxes sockets, GPU/audio/network, PolicyKit, and the
Flatpak host service. Fixed `/usr/bin/flatpak` and `/usr/bin/virt-viewer` host
wrappers support existing discovery/import and viewer fallback. The Flatpak
host service permission permits host command execution; this is a desktop VM
manager with broad host access. dconf access preserves the existing
`/io/github/pirwa/Pirwa/` preference storage shared with native installs.
App-owned new files use Flatpak's normal application data directories.

## Automated release

`.github/workflows/release.yml` runs on `v*` tag pushes and manual dispatch.
Manual runs build and upload artifacts only. Each package job consumes the
same checksummed source archive; all run the default headless Meson tests.
DEB and RPM use clean containers with normal dependencies and networking
disabled during package builds. Flatpak builds against its SDK with pinned
dependency downloads. The assembly job rejects missing formats, different
source snapshots, stale output, changed files and checksum mismatches.

Before the first tag:

1. Check the GitHub **release** environment, preferably with a required
   reviewer for the final release decision.
2. Set its `COPR_CONFIG` secret to the complete copr-cli configuration from an
   account permitted to build `stonegate/pirwa`. Obtain credentials from the
   [COPR API page](https://copr.fedorainfracloud.org/api/); do not commit them.
   CI writes a private temporary config, runs the CLI, then removes it.
3. Create the COPR project once from the authenticated account:

   ```sh
   copr-cli whoami
   copr-cli create pirwa --chroot fedora-45-x86_64 --appstream on \
     --description 'Native GNOME virtual machine manager using libvirt' \
     --instructions 'Install with: sudo dnf copr enable stonegate/pirwa && sudo dnf install pirwa'
   ```

The publish job requires a clean stable version, checks that source provenance
matches the triggering commit/tag, submits the verified SRPM through copr-cli,
and waits for the Fedora build to succeed. Only then does GitHub CLI create a
release with every verified asset and the version's committed release notes.
A failed COPR build prevents GitHub
publication. An existing GitHub release is not overwritten. Check COPR and any
remaining draft before retrying a failed publish job; a retry can submit another
COPR build. Do not delete or retag a public release to retry it.

## Local builds

Use Python 3.12 or newer, Git, xz, and the native package tools. All commands
run from the repository root. Output directories must be empty. A stable
release uses a clean committed tree and its matching tag:

```sh
python3 scripts/release.py source --tag v0.1.0 --output dist/source
python3 scripts/release.py deb --source dist/source/source.json --output dist/deb
python3 scripts/release.py rpm --source dist/source/source.json --output dist/rpm
flatpak remote-add --user --if-not-exists flathub https://flathub.org/repo/flathub.flatpakrepo
flatpak install --user flathub org.gnome.Sdk//51 org.gnome.Platform//51
python3 scripts/release.py flatpak --source dist/source/source.json --work flatpak-build --output dist/flatpak
python3 scripts/release.py manifest --source dist/source/source.json --input dist --output build/release-assets
python3 scripts/release.py verify --input build/release-assets --publish
python3 scripts/release.py copr --input build/release-assets --dry-run
python3 scripts/release.py copr --input build/release-assets --project stonegate/pirwa
```

Run DEB and RPM commands inside their corresponding build containers or hosts;
see `packaging/Containerfile.deb` and `packaging/Containerfile.qa`. Feed both the
same source archive and `source.json`. The generated SRPM stores its full source
version in its spec, so COPR needs no local `--define` overrides. The canonical
source filename is retained for stable and development versions.

For uncommitted local QA, use `source --allow-dirty` without `--tag` and new
output directories. Such packages are marked dirty in provenance and cannot
pass the publishing check. `copr --dry-run` prints the submission command without
network access or credentials. No packaging command tags or pushes Git commits.

References: [Flatpak sandbox permissions](https://docs.flatpak.org/en/latest/sandbox-permissions.html),
[COPR CLI](https://github.com/fedora-copr/copr/blob/main/cli/copr_cli/README.rst),
[GitHub release creation](https://cli.github.com/manual/gh_release_create).
