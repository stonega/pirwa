# SPDX-License-Identifier: GPL-3.0-or-later
"""QA runner checks; requires the same Python GObject bindings as the profiler."""

import importlib.util
import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

spec = importlib.util.spec_from_file_location(
    "console_profile",
    Path(__file__).resolve().parents[1] / "scripts/profile-console.py",
)
profile = importlib.util.module_from_spec(spec)
with patch.object(sys, "dont_write_bytecode", True):
    spec.loader.exec_module(profile)


class FramebufferReadiness(unittest.TestCase):
    def wait(self, schedule, *, timeout=30, exit_at=None, advance=None):
        clock = SimpleNamespace(now=0.0)

        def sleep(seconds):
            clock.now += seconds if advance is None else advance

        ready = SimpleNamespace(
            read_text=lambda: json.dumps(schedule(clock.now)[1]),
            stat=lambda: SimpleNamespace(
                st_ino=schedule(clock.now)[0], st_mtime_ns=schedule(clock.now)[0]
            ),
        )
        process = SimpleNamespace(
            poll=lambda: 1 if exit_at is not None and clock.now >= exit_at else None
        )
        with (
            patch.object(profile.time, "monotonic", lambda: clock.now),
            patch.object(profile.time, "sleep", sleep),
        ):
            return profile.wait_for_framebuffer(
                ready, (3840, 2160), process, timeout=timeout
            )

    def test_surface_recreation_restarts_quiet_window(self):
        state = {"agent_connected": True, "width": 3840, "height": 2160}
        result = self.wait(lambda t: (1 if t < 2 else 2, state))
        self.assertGreaterEqual(result["seconds"], 7)
        self.assertEqual(result["ready_versions_observed"], 2)

    def test_expected_size_and_agent_are_required(self):
        def schedule(t):
            return int(t >= 2) + int(t >= 4), {
                "agent_connected": t >= 2,
                "width": 3840 if t >= 4 else 1280,
                "height": 2160 if t >= 4 else 800,
            }

        self.assertGreaterEqual(self.wait(schedule)["seconds"], 9)

    def test_continual_recreation_times_out(self):
        state = {"agent_connected": True, "width": 3840, "height": 2160}
        with self.assertRaisesRegex(RuntimeError, "did not settle"):
            self.wait(lambda t: (int(t), state), timeout=6)

    def test_exited_client_fails_without_waiting_for_deadline(self):
        state = {"agent_connected": True, "width": 3840, "height": 2160}
        with self.assertRaisesRegex(RuntimeError, "Console exited"):
            self.wait(lambda t: (1, state), exit_at=2)

    def test_host_stall_cannot_bypass_deadline(self):
        state = {"agent_connected": True, "width": 3840, "height": 2160}
        with self.assertRaisesRegex(RuntimeError, "did not settle"):
            self.wait(lambda t: (1, state), advance=31)


class MemoryHeadroom(unittest.TestCase):
    def test_guest_with_host_margin_is_allowed(self):
        with patch.object(profile.Path, "read_text", return_value="MemAvailable: 4194304 kB\n"):
            self.assertEqual(profile.memory_headroom(3072)["required_kib"], 4194304)

    def test_low_memory_is_rejected_before_resources(self):
        with (
            patch.object(profile.Path, "read_text", return_value="MemAvailable: 3145728 kB\n"),
            self.assertRaisesRegex(RuntimeError, "No test resources were created"),
        ):
            profile.memory_headroom(3072)

    def test_missing_or_invalid_available_memory_fails_closed(self):
        for contents in ("MemTotal: 33554432 kB\n", "MemAvailable: bad kB\n", "MemAvailable:\n"):
            with (
                self.subTest(contents=contents),
                patch.object(profile.Path, "read_text", return_value=contents),
                self.assertRaisesRegex(RuntimeError, "Cannot determine"),
            ):
                profile.memory_headroom(3072)


if __name__ == "__main__":
    unittest.main()
