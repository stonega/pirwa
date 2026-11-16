# Install a Windows virtual machine

Windows 10 and Windows 11 x86_64 installation profiles are experimental. Their
configuration and synthetic provisioning tests pass; a full Windows installation
has not yet been validated. Pirwa's host remains Linux with QEMU/KVM. Windows
Server, older Windows versions, unattended setup, and Windows disk/TPM migration
are outside this workflow.

## Prepare the host and media

Use a local x86_64 Windows installation ISO and a separate signed virtio-win
driver ISO. The [VirtIO project's installation guide](https://virtio-win.github.io/Knowledge-Base/Driver-installation.html)
links the driver download and explains manual driver installation. Pirwa does not
download these images.

For Windows 11 on Fedora, install the optional host packages:

```sh
sudo dnf install edk2-ovmf swtpm swtpm-tools
```

The libvirt connection must report usable UEFI firmware and emulated TPM 2.0.
Windows 11 requires Secure Boot firmware with enrolled keys; Pirwa reports a
creation error if it cannot select that firmware. The host CPU must also meet
[Microsoft's Windows 11 VM requirements](https://learn.microsoft.com/en-us/windows/whats-new/windows-11-requirements#virtual-machine-support).
Pirwa exposes the host CPU; it does not certify its eligibility.

## Create and install

1. Choose **New Virtual Machine** and select the Windows installation ISO.
2. Name the machine and continue to **Config**.
3. Under **Operating system**, choose **Windows 10** or **Windows 11**. Choose or
   drop the local virtio-win ISO on **VirtIO Driver ISO**.
4. Review the resources. Both profiles initially offer at least 2 CPUs, 4096 MiB
   RAM and a 64 GiB disk. Windows 11 enforces those minimums and disables legacy
   BIOS. The Windows 10 profile allows 1 CPU, 2048 MiB and 32 GiB if adjusted.
5. Review **Boot**, then choose **Create and Boot**. If prompted by the Windows
   DVD boot screen, press a key in the viewer.
6. In Windows Setup's disk selection screen, choose **Load driver**. Browse the
   driver CD's `viostor/w11/amd64` directory for Windows 11, or
   `viostor/w10/amd64` for Windows 10. Pirwa uses VirtIO block storage, so load
   `viostor`. Select the newly visible disk and continue installation.
7. Install the matching **NetKVM** driver for networking. If first-run setup needs
   a network driver before the desktop, use its driver installation option. From
   the setup command prompt (Shift+F10), the following is an example for Windows
   11 when the driver CD is `E:`:

   ```bat
   pnputil /add-driver E:\NetKVM\w11\amd64\netkvm.inf /install
   ```

   Use the actual CD drive letter, and `w10` for Windows 10. Microsoft's
   [PnPUtil documentation](https://learn.microsoft.com/en-us/windows-hardware/drivers/devtest/pnputil-command-syntax#add-driver)
   describes installing a driver package for matching devices.
8. At the desktop, run the driver CD's guest-tools installer and reboot when
   requested. Confirm the disk, network and VirtIO serial drivers in Device Manager.
   Automatic display resizing also needs a working SPICE guest agent.
9. Eject the Windows installer with Pirwa's filename-and-eject button after
   installation. The driver CD then appears as the next mounted medium and can
   be ejected after installing the drivers. The stopped-machine **Eject installation
   media** setting removes both CDs.

Keep the two ISO files at their original paths until ejected. Windows 11 retains
its TPM state and firmware variables between starts. The console uses software
rendering; DirectX/3D acceleration and multi-monitor support are outside this path.

## Keep an installed disk safely

For a TPM-backed VM, **Remove** with disk deletion disabled keeps the disk, TPM
state and firmware variables. Deleting a verified Pirwa-owned disk also removes
its TPM and firmware state. Preserve the original domain XML and all state for
recovery; a disk file alone does not preserve the identity of an encrypted VM.
Existing VMs are discovered without changing their hardware. Importing a Windows
qcow2 image through these new installation profiles is intentionally unavailable.

See the [validation record](../research/windows-guests/README.md) for completed
checks and the real-guest acceptance work still required.
