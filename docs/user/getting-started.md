# Getting started

Pirwa is at the project-scaffold stage. Build it using the README, then run
`./examples/run-test-driver.sh` to see a synthetic VM without configuring KVM.

For your own VMs, close Pirwa and launch `./build/src/pirwa`. It lists the domains
visible to your user on `qemu:///session`. Choose Refresh to read current states.
Power controls, creation, resource editing, and console access are not available
yet. Viewing a VM does not change it.

To inspect system VMs, close the app and run
`PIRWA_LIBVIRT_URI=qemu:///system ./build/src/pirwa`. Your normal libvirt access
permissions apply. The session and system inventories are different.

If the connection fails, check that the requested libvirt instance is available
and that your user has access, then choose Refresh. An empty list means there are
no visible domains on that connection. It does not scan other connections.
