# Application identity and release metadata

The 2026-11-16 follow-up replaces the provisional `io.github.pirwa.Pirwa`
application identity with `io.github.stonega.Pirwa`. Read-only connected GitHub
queries identify the authenticated account as `stonega`, the owner of
`stonega/pirwa`, with administrator permission. The repository is private.
No repository setting or remote content was changed.

## Identity and preferences

The application ID now agrees with the desktop entry, AppStream component and
launchable, developer namespace, installed icon names, embedded resource paths,
GSettings schema ID, RPM file list and QA D-Bus clients. Asset contents were
preserved when their filenames changed. The embedded SVG and symbolic SVG are
listed from the actual executable's GResource section.

GSettings deliberately retains `/io/github/pirwa/Pirwa/` as its storage path.
The compatibility fixture uses the old schema ID against the same in-memory
backend and verifies proxy routing, DNS resolver, display resize and per-URI VM
selection through the new schema, including updates back through the old view.
The old schema is test-only and is not installed. Startup needs no migration
write, and VM ownership records retain their established locations.

## Metadata and packaging

The metainfo contains the canonical project/issue/source URLs and a dated
`0.1.0-dev` snapshot entry. It does not announce a stable release. A genuine
1088×680 application screenshot shows three stopped synthetic test-driver VMs;
no real guest or fabricated guest screen is included. The final capture waits
through the existing QA settling period so resource details are visible. The
initial early capture is retained as a diagnostic, not used as the catalog asset.
See [capture instructions](../../../data/screenshots/README.md) and the
[runnable fixture](../../../examples/screenshot-machines.xml).

Strict offline AppStream validation now passes without the old missing-homepage
severity override. A capitalized-component-ID pedantic hint remains; it does not
fail strict validation. Complete staged and RPM trees also pass validation of
the metadata, desktop file and installed icons. RPM builds now require the
AppStream validation tool; no application runtime library was added.

The headless metadata contract checks version agreement with Meson and the RPM
base version, desktop/icon/schema IDs, resource paths and screenshot dimensions.
Negative controls reject a wrong desktop icon, stale release version and missing
PNG. RPM source archives and extraction prefixes take Meson's source version
through `pirwa_source_version`; queries verify both `0.1.0-dev` and `0.1.0` forms.
CI and the native build script pass this macro and no longer assume the initial
archive or installed RPM filename. A stable package still needs deliberate
Version/Release and AppStream kind/date promotion after acceptance.

## Verified scope

- 21 normal and 21 leak-enabled ASan/UBSan headless checks pass. The existing
  narrow libvirt initialization exception is unchanged.
- Ten normal and ten explicit Intel-Vulkan sanitizer desktop cases pass with
  the new identity, including menu recycling, dialogs, preferences, console
  handoff, Boxes fixtures and object finalization.
- The unchanged production entry point completes one warm-up and one smoke
  launch with the new ID, three synthetic domains and complete presentation
  feedback. These two launches do not establish a performance percentile.
- The offline rootless Fedora check uses normal RPM dependencies, passes 21
  checks per normal/sanitizer build, validates the staged tree, and builds,
  installs and verifies the development RPM. The package file list uses the new
  identity. No system package or existing guest on the host was changed.
- Formatting and `git diff --check` pass.

The developer host still uses its extracted SDK. The clean package check is
separate evidence for normal distribution dependencies. GUI sanitizer leak
detection is disabled as documented in the [GUI lifetime assessment](../gui-lifetimes/README.md);
its toolkit/font-stack heap findings remain open. The desktop test accessibility
backend is explicit, so physical input and screen-reader acceptance remain open.

Concurrent Windows-profile edits appeared in the working tree after the package
snapshot. They were preserved. The final desktop matrices run the frozen native
package-check executables and their matching schemas; the accepted headless logs
and source archive come from that same snapshot. The validation file records
later source drift. These results do not validate the later Windows work or the
entire current working tree.

The first exported-binary desktop attempt also exposed a QA setup gap: the
wrapper did not select the schema shipped with `--build`, so it reached the
binary's unavailable `/work/build/data` development fallback. The standard
matrix now sets the selected build's schema directory and records its hash;
custom-command schema handling is unchanged. The failed attempt is retained.
The corrected wrapper is archived separately under `verified-qa/` because it
postdates the frozen application/package source snapshot.

## Reproduce and publication boundary

```sh
meson compile -C build
meson test -C build --print-errorlogs
appstreamcli validate --strict --no-net data/io.github.stonega.Pirwa.metainfo.xml
./scripts/test-gnome.py --build build --output build/qa/metadata-ui-new
./scripts/check-clean-build.py --rpm --output build/qa/metadata-package-new
```

Use installed dependencies or the local SDK setup documented in
[development setup](../../implementation/setup.md). The native package command
uses the existing rootless QA image; add `--build-image` to prepare it when needed.

The repository and screenshot are not publicly published. `--no-net` checks
metadata structure and local integration; it cannot prove that anonymous users
can open the homepage or fetch the image URL. Public URL/image reachability,
committed-candidate CI, stable promotion, final review and all other acceptance
gates in the [release checklist](../../release/v0.1.0-readiness.md) remain open.
No tag or public release was created.

[Validation](validation.json), [compressed raw results](raw-results.json.gz) and
the [source archive](sources.tar.gz) retain the ownership verification, exact
inputs and hashes, controls, screenshot captures, desktop outcomes, filtered
headless logs and clean-package evidence. Historical evidence retains its old
application IDs and source snapshots rather than being rewritten.
