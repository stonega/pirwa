# Libvirt event dispatch on the wrong thread

During implementation, the backend test asserted that an async completion ran
on the caller thread. It instead ran on a dedicated event-loop thread.

The installed libvirt default event implementation used GLib's default context.
Driving that loop on a separate thread let it dispatch sources belonging to the
desktop main context, including GTask completions.

Pirwa now registers libvirt-glib's GNOME event adapter before connecting, and
the normal main loop dispatches events. Callbacks enqueue work; blocking libvirt
operations remain serialized on the worker. The default test suite asserts
callback affinity and checks event-driven reconciliation to prevent recurrence.
