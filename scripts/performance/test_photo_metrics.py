#!/usr/bin/env python3
import tempfile
import unittest
from pathlib import Path
from photo_scroll import metrics, trace_completed


class PhotoMetricsTests(unittest.TestCase):
    def parse(self, lines):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "trace"
            p.write_text(lines)
            return metrics(p)

    def test_completion_marker_must_be_complete_and_last(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "trace"
            self.assertFalse(trace_completed(p))
            for text in (
                "",
                "done 123",
                "done nope\n",
                "done 123\nframe 124 1 1 0 1\n",
            ):
                p.write_text(text)
                self.assertFalse(trace_completed(p))
            p.write_text("frame 120 1 1 0 1\ndone 123\n")
            self.assertTrue(trace_completed(p))

    def test_ready_requires_matching_sequence_and_cursor(self):
        result = self.parse(
            "key 100 1 1 0\nframe 110 1 1 0 1\nkey 200 2 1 2\nframe 210 1 1 0 1\nframe 220 2 1 0 1\nframe 230 2 1 2 0\nkey 300 3 1 4\nframe 310 2 1 2 1\nframe 320 3 1 4 1\ndone 330\n"
        )
        self.assertEqual(
            [r["preview_ready"] for r in result["transitions"]], [True, False, True]
        )
        self.assertEqual(result["phases"][0]["preview_not_ready_before_next_input"], 1)
        self.assertTrue(result["transitions"][1]["presented"])

    def test_unrendered_selection_is_distinct_from_missing_pixels(self):
        result = self.parse(
            "key 100 1 1 0\nkey 200 2 1 2\nframe 230 2 1 2 0\ndone 300\n"
        )
        self.assertFalse(result["transitions"][0]["presented"])
        self.assertTrue(result["transitions"][1]["presented"])
        self.assertFalse(result["transitions"][1]["preview_ready"])

    def test_background_reorder_cannot_match_different_photo_at_same_cursor(self):
        result = self.parse(
            "key 100 1 1 5 photo:10\nframe 110 1 1 5 1 photo:11\nframe 120 1 1 5 0 photo:10\ndone 130\n"
        )
        self.assertFalse(result["transitions"][0]["preview_ready"])
        self.assertTrue(result["transitions"][0]["presented"])

    def test_last_key_cannot_use_long_idle_to_hide_a_miss(self):
        result = self.parse("key 100 1 1 5\nframe 50000100 1 1 5 1\ndone 60000100\n")
        self.assertFalse(result["transitions"][0]["preview_ready"])

    def test_identity_is_cursor_not_duplicate_filename(self):
        result = self.parse("key 100 1 1 5\nframe 120 1 1 6 1\ndone 130\n")
        self.assertFalse(result["transitions"][0]["preview_ready"])

    def test_refresh_only_workload_counts_completed_queries(self):
        result = self.parse(
            "draw 10 0 10 -1 -1 3 30834\n"
            "refresh_begin 20 1 30834\n"
            "refresh_done 30 1 30834\n"
            "refresh_begin 40 2 30834\n"
            "refresh_done 50 2 30834\n"
            "done 60\n"
        )
        self.assertEqual(result["refreshes_started"], 2)
        self.assertEqual(result["refreshes_completed"], 2)
        self.assertEqual(result["max_loaded"], 30834)
        self.assertEqual(result["transitions"], [])
        self.assertTrue(result["done"])


if __name__ == "__main__":
    unittest.main()
