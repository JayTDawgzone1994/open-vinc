"""Check measurement math independently of the live transport."""

import tempfile
import unittest
from pathlib import Path

import run


class AnalysisTests(unittest.TestCase):
    def test_nearest_rank(self):
        samples = list(range(100, 0, -1))
        self.assertEqual(run.percentile(samples, 0.50), 50)
        self.assertEqual(run.percentile(samples, 0.90), 90)
        self.assertEqual(run.percentile(samples, 0.99), 99)

    def fixture(self, directory):
        run.write_csv(directory / "sent.csv", [
            {"sequence": 1, "prepare_ns": 1, "begin_ns": 100, "end_ns": 110},
            {"sequence": 2, "prepare_ns": 2, "begin_ns": 200, "end_ns": 220},
            {"sequence": 3, "prepare_ns": 3, "begin_ns": 300, "end_ns": 330},
        ])
        run.write_csv(directory / "received.csv", [
            {"sequence": 1, "arrival_ns": 1000, "valid": 1},
            {"sequence": 2, "arrival_ns": 250, "valid": 1},
        ])
        run.write_csv(directory / "publisher.csv", [{
            "shared_bytes": 4096, "image_bytes": 256, "slots": 2, "published": 3,
            "collisions": 0, "total_wait_ns": 0, "longest_wait_ns": 0,
        }])
        run.write_csv(directory / "subscriber.csv", [{"lost_messages": 0, "overwrite_events": 0}])
        for role in ("publisher", "subscriber", "central"):
            run.write_csv(directory / f"{role}_resources.csv", [{
                "valid": 1, "cpu_ns": 500, "wall_ns": 1000, "peak_rss_bytes": run.MIB,
            }])

    def test_missing_tail_is_not_hidden_by_framework_loss_counter(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            result = run.analyze(directory, "push", 1, 2, 100, 2, 1)
            self.assertEqual(result["received"], 1)
            self.assertEqual(result["missing_measured"], 1)
            self.assertFalse(result["clean"])
            self.assertEqual(result["latency_p99_ms"], 50 / 1e6)
            self.assertEqual(result["full_path_p99_ms"], 52 / 1e6)
            self.assertEqual(result["publisher_cpu_percent_one_core"], 50)

    def test_capacity_includes_topic_counter(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            publisher = run.rows(directory / "publisher.csv")[0]
            publisher["shared_bytes"] = run.SHARED_BUDGET
            run.write_csv(directory / "publisher.csv", [publisher])
            with self.assertRaisesRegex(RuntimeError, "4 GiB"):
                run.analyze(directory, "push", 1, 2, 100, 2, 1)

    def test_negative_latency_is_integrity_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            run.write_csv(directory / "received.csv", [
                {"sequence": 1, "arrival_ns": 120, "valid": 1},
                {"sequence": 2, "arrival_ns": 190, "valid": 1},
                {"sequence": 3, "arrival_ns": 340, "valid": 1},
            ])
            result = run.analyze(directory, "loan", 1, 2, 100, 2, 1)
            self.assertEqual(result["integrity_errors"], 1)
            self.assertFalse(result["clean"])

    def test_excess_callback_cannot_disappear_at_sample_capacity(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            self.fixture(directory)
            run.write_csv(directory / "subscriber.csv", [{
                "lost_messages": 0, "overwrite_events": 0, "excess_callbacks": 1,
            }])
            result = run.analyze(directory, "push", 1, 2, 100, 2, 1)
            self.assertEqual(result["integrity_errors"], 1)
            self.assertFalse(result["clean"])


if __name__ == "__main__":
    unittest.main()
