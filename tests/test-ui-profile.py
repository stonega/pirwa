# SPDX-License-Identifier: GPL-3.0-or-later
"""Guard the evidence accepted for compositor presentation percentiles."""
import importlib.util
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("profile_ui", Path(__file__).resolve().parents[1] / "scripts/profile-ui.py")
profile = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profile)


def sample(rows, dropped=0):
    return {"sample_start_monotonic_us": 1000000, "sample_end_monotonic_us": 1100000,
            "feedback_dropped": dropped,
            "presentation_feedback": {"columns": ["frame_counter", "before_paint_us", "after_paint_us",
                                                    "damage_received_us", "complete", "presentation_us",
                                                    "refresh_interval_us", "result"], "samples": rows}}


class PresentationEvidence(unittest.TestCase):
    def test_only_complete_presented_frames_inside_window_are_eligible(self):
        rows = [[1, 1010000, 1011000, 0, True, 1013000, 16667, "presented"],
                [2, 1020000, 1021000, 0, True, 1032000, 16667, "presented"],
                [3, 1030000, 1031000, 0, False, 1200000, 16667, "presented"],
                [4, 1040000, 1041000, 0, True, 1300000, 16667, "discarded"],
                [5, 990000, 991000, 0, True, 1500000, 16667, "presented"]]
        result = profile.presentation_summary(sample(rows))
        self.assertEqual(result["accepted_presentations"], 2)
        self.assertEqual(result["before_paint_to_presentation_p95_ms"], 12)
        self.assertEqual(result["presentation_interval_p95_ms"], 19)
        self.assertEqual(result["feedback_results"], {"presented": 3, "discarded": 1})

    def test_unavailable_and_invalid_timestamps_cannot_pass_as_zero_latency(self):
        rows = [[1, 1010000, 1011000, 0, True, 0, 0, "outstanding"],
                [2, 1020000, 1021000, 0, True, 1019999, 16667, "presented"]]
        result = profile.presentation_summary(sample(rows))
        self.assertEqual(result["accepted_presentations"], 0)
        self.assertIsNone(result["before_paint_to_presentation_p95_ms"])
        self.assertIsNone(result["presentation_interval_p95_ms"])

    def test_truncated_feedback_cannot_support_a_percentile(self):
        with self.assertRaisesRegex(RuntimeError, "Truncated"):
            profile.presentation_summary(sample([], dropped=1))


if __name__ == "__main__":
    unittest.main()
