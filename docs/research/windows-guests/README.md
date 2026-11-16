# Windows guest profiles — 2026-11-16

Windows 10/11 x86_64 ISO creation now has explicit experimental profiles. No
Windows installation ISO was available during this work, so real guest
installation remains an acceptance gate. Synthetic success is not a Windows
compatibility certification.

## Design and sources

- [Microsoft's VM requirements](https://learn.microsoft.com/en-us/windows/whats-new/windows-11-requirements#virtual-machine-support)
  specify Windows 11 VM memory, storage, processor and security requirements.
  Pirwa enforces 2 vCPUs, 4096 MiB and 64 GiB at creation. Host CPU eligibility
  remains the user's responsibility.
- [Libvirt Secure Boot](https://www.libvirt.org/kbase/secureboot.html) documents
  requesting `secure-boot` and `enrolled-keys` during firmware autoselection.
- [Domain capabilities](https://www.libvirt.org/formatdomaincaps.html) documents
  firmware support and the TPM model/backend/version enums. Firmware feature
  enums were added in libvirt 12.1; older hosts rely on definition-time firmware
  selection and rollback rather than a guessed firmware path.
- [Libvirt domain XML](https://www.libvirt.org/formatdomain.html#tpm-device)
  documents emulated TPM 2.0 and persistent state. TPM state lives with libvirt;
  Pirwa does not launch swtpm itself.
- [VirtIO driver installation](https://virtio-win.github.io/Knowledge-Base/Driver-installation.html)
  explains supplying a driver ISO and loading storage drivers in Windows Setup.
  Pirwa retains VirtIO disk/network devices and attaches a second read-only
  SATA CD, rather than switching the disk controller after installation.

Optional Fedora runtime packages are edk2-ovmf, swtpm and swtpm-tools. No build-time
library or additional GUI runtime is introduced. Windows 11 uses Secure Boot,
enrolled keys, SMM, tpm-crb/2.0 and persistent TPM state. Windows 10 can use legacy
BIOS and does not require TPM. Both use a local-time clock and one CPU socket.
Known ISO/profile mismatches fail before disk allocation. Unknown ISO identities
can use the explicit profile; libosinfo still checks the ISO format/bootability.

## Repeatable checks

```sh
meson compile -C build
meson test -C build --print-errorlogs
clang-format --dry-run --Werror src/*.[ch] tests/*.c
./scripts/test-gnome.py --output build/qa/windows-create -- \
  ./build/tests/test-create-ui --output build/qa/windows-create/screenshots
./scripts/test-gnome.py --output build/qa/windows-create-accessible -- \
  ./build/tests/test-create-ui --output build/qa/windows-create-accessible/screenshots \
  --dark --large-text --width 720
```

`config` covers Windows resources, firmware/TPM XML, two independent CD sources,
escaping, Linux defaults, host capability rejection and preserving devices during
resource edits. `windows-provision` wraps capabilities/media identification and
uses real test-driver domains/storage metadata. It exercises successful creation,
start failure with rollback, source architecture/profile/driver errors before
allocation, and keeping/deleting TPM/firmware state. The test driver creates no
QEMU or TPM process; adapters record the requested flags and provide private
tiny inodes for production ownership checks. The desktop fixture checks profile
selection, driver-file validation, resource presets, BIOS restriction, review,
back navigation and submitted options, along with existing Linux/import flows.

Results on this development host:

- Native build, all 22 default tests, full C formatting check and Git whitespace
  check passed.
- Both desktop commands passed all four creation cases, at normal size and at
  720 px with dark appearance and 200% text. Screenshots and reports are under
  `build/qa/windows-create` and `build/qa/windows-create-accessible`.
- `config` and `windows-provision` passed with address/undefined sanitizers and
  leak detection in `build-windows-asan`. This used the documented local SDK
  runtime path and existing narrow libvirt initialization suppression; no new
  suppression was added. The log is
  `build-windows-asan/meson-logs/testlog.txt`.

## Remaining real-guest acceptance

Use uniquely named disposable VMs and local user-supplied images. Keep the
profiles experimental until these checks have evidence:

- Windows 11: boot with Secure Boot/enrolled keys and TPM 2.0; inspect their
  status inside Windows, install signed viostor/NetKVM/serial/SPICE drivers,
  finish first-run networking and reopen the installed disk after ISO eject.
- Windows 10: installation and installed-disk boot with the matching signed
  driver ISO, including the supported firmware choices.
- Confirm keyboard, pointer, audio, agent resizing, restart, graceful shutdown,
  host proxy routing and daemon/app restart continuity on Windows.
- Exercise Windows 11 TPM/NVRAM persistence through restart, failed creation,
  and explicit removal with disk kept/deleted. Check an encrypted disposable
  guest separately before claiming recovery coverage.
- Verify host package/firmware combinations and collect guest CPU/storage
  measurements before making performance claims. QXL/software rendering does
  not establish DirectX 12/3D or Microsoft hardware compatibility.
