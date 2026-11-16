# Clean Fedora native build

On 2026-11-15, a frozen source snapshot passed the optimized native build,
18 headless checks, an ASan/UBSan build with 18 checks, staged installation,
strict Fedora RPM build and its 18 checks, offline RPM installation and
`rpm -V`. The installed executable reported **Pirwa 0.1.0-dev**. This is a
development package validation, not the v0.1.0 release decision.

All headers and runtime libraries were installed as normal Fedora 45 RPMs in a
rootless Podman image. No extracted SDK, BuildRequires bypass or privileged
container was used. Dependency installation alone used the host network stack
to reach this environment's loopback package proxy. Builds/tests/install ran
with networking disabled and an explicit clean environment. The container had
only a read-only source snapshot and a writable result directory mounted.

The first attempt reached RPM compilation and failed at two ignored `write()`
results in the new gateway helper. Fedora's fortified compiler treats those as
errors. The signal/worker wakeups now consume the result and retry EINTR; a full
nonblocking wake pipe still means poll is already awake. The signal handler's
errno preservation remains intact. No new timer or polling was added.

The minimal QEMU runtime also lacked the storage daemon/directory-pool backend.
Pirwa now explicitly requires `libvirt-daemon-driver-storage-core`; VM disk
creation must not depend on optional package recommendations. The validation
image installs the declared runtime dependencies, and its package installation
uses disabled repositories to verify the built RPM needs no undeclared download.

The [validation record](validation.json) retains the frozen source manifest,
image ID, installed package versions, binary/RPM hashes, test results, initial
compiler failure and final outcomes. Concurrent source changes after the snapshot
remain listed; this does not sign off a newer working tree. CI now installs Git
before checkout and builds the RPM with normal BuildRequires, so the fortified
compile is covered alongside the existing normal/sanitizer tests.

The copied QA executables also need the matching compiled GSettings schema.
The initial out-of-container guest probe omitted that resource and aborted on
preferences initialization. The test tools now pin both, use an in-memory
settings backend, and reject incomplete build bundles. The resource sidecar
records schemas compiled from the frozen XML in the same image. The
[Linux guest checks](../backend-recovery/README.md#running-linux-guest-follow-up)
then passed all six cases for both native and sanitizer clients.

```sh
./scripts/check-clean-build.py --build-image --rpm \
  --output build/qa/clean-native-new
```

Add `--build-network host` only for a package proxy bound to host loopback.
The test image and results remain local. These checks do not certify desktop
interaction, KVM provisioning in the container, host policy, performance,
final application identity/metadata or publication. Leak detection remains
disabled for the recorded initialization/TLS findings.
