# Closing a modal stopped the VM backend

The first native Boxes onboarding preview displayed correctly but its QA process
timed out after attempting to close the window. The main window remained mapped.

Pirwa cancelled its lifetime and stopped the backend on every close-request.
Libadwaita can consume a window close request to dismiss the visible modal
dialog while keeping the application window open. The cancellation therefore
left a visible window with a stopped backend. The new onboarding made this
existing lifecycle assumption observable.

The close-request handler now closes the visible dialog first and returns
without cancelling window work. A busy dialog can reject that close normally.
When no dialog is present, the actual window close cancels work. Disposal calls
the cancellation helper directly so forced teardown always stops outstanding
work. The profiling helper explicitly destroys a window when ending a preview
with a modal still open; that path is distinct from testing normal UI closing.

The 200% dark/narrow Boxes scenario now sends a window close request while the
guide is open, waits for the remembered dismissal, reopens the window/guide,
and verifies that the existing VM remains controllable and is not duplicated.
The full virtual GNOME matrix and sanitizer matrix pass, and the actual native
library preview exits cleanly after rendering both host Boxes VMs.
