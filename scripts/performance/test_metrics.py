"""Regression tests for benchmark interpretation, independent of a GUI/GPU."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('performance', Path(__file__).with_name('run.py'))
performance = importlib.util.module_from_spec(spec)
spec.loader.exec_module(performance)


def key(ns, sequence):
    return {'kind': 'key', 'ns': ns, 'sequence': sequence, 'symbol': 65364}


def frame(ns, sequence, name, busy=0, moving=0):
    return {'kind': 'frame', 'ns': ns, 'sequence': sequence, 'selected': name, 'busy': busy, 'moving': moving}


class MetricsTest(unittest.TestCase):
    def test_waits_for_correct_loaded_and_settled_frame(self):
        events = [key(101, 2), frame(110, 1, 'sample-00000.c'),
                  frame(120, 2, 'sample-00001.c', busy=1),
                  frame(130, 2, 'sample-00001.c', moving=1),
                  frame(140, 2, 'sample-00001.c')]
        result = performance.preview_metrics([100], events, 'sample', 'c', 200)
        self.assertEqual(result['transitions'][0]['settled_ns'], 140)
        self.assertEqual(result['transitions'][0]['loaded_ns'], 130)
        self.assertEqual(result['unadvanced_selection'], 0)
        self.assertEqual(result['missed_delivery'], 0)

    def test_received_but_unhandled_is_not_a_delivery_failure(self):
        result = performance.preview_metrics([100], [key(101, 2), frame(140, 2, 'sample-00000.c')], 'sample', 'c', 200)
        self.assertEqual(result['missed_delivery'], 0)
        self.assertEqual(result['unadvanced_selection'], 1)
        self.assertEqual(result['unsettled_or_wrong_selection'], 1)
        self.assertIsNone(result['latency_ms'])

    def test_dropped_key_is_separate_from_subsequent_wrong_selection(self):
        events = [key(201, 2), frame(230, 2, 'sample-00001.c')]
        result = performance.preview_metrics([100, 200], events, 'sample', 'c', 300)
        self.assertEqual(result['missed_delivery'], 1)
        self.assertEqual(result['unadvanced_selection'], 1)
        self.assertEqual(result['unsettled_or_wrong_selection'], 2)

    def test_late_frame_cannot_be_counted_for_previous_key(self):
        events = [key(101, 2), key(201, 3), frame(220, 2, 'sample-00001.c'), frame(240, 3, 'sample-00002.c')]
        result = performance.preview_metrics([100, 200], events, 'sample', 'c', 300)
        self.assertIsNone(result['transitions'][0]['latency_ms'])
        self.assertEqual(result['transitions'][1]['settled_ns'], 240)

    def test_trace_tolerates_only_incomplete_last_line(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'trace'
            path.write_text('key 100 2 65364\ndraw 120 2 0 0 sample-00001.c 1200 940\nframe 130 2 0 0 sample-00001.c\nframe 14')
            events = performance.trace(path)
        self.assertEqual(len(events), 3)
        self.assertEqual(events[1]['size'], [1200, 940])

    def test_percentile_uses_nearest_rank_and_preserves_samples(self):
        values = list(range(1, 101))
        result = performance.distribution(values)
        self.assertEqual(result['p95'], 95)
        self.assertEqual(result['median'], 50.5)
        self.assertEqual(result['max'], 100)
        self.assertEqual(result['samples'], values)


if __name__ == '__main__':
    unittest.main()
