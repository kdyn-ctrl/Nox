"""
Tests for insider_cluster.py's recency filter (audit §6 H6, Track 3 burndown).

Before this fix, `_find_cluster` only checked that buys landed within
WINDOW_HOURS of EACH OTHER — a years-old buy-cluster in a thin filer would
re-emit from every scan (fetch_form4_filings just returns whatever the SEC
feed has) and boost sizing 1.25x forever, presented as current conviction.
`_find_cluster` now discards any buy older than INSIDER_CLUSTER_MAX_AGE_DAYS
before clustering.
"""
import os

os.environ.setdefault("ALPACA_API_KEY", "test")
os.environ.setdefault("ALPACA_SECRET_KEY", "test")

import unittest
from datetime import datetime, timedelta, timezone

import insider_cluster as ic


def _buy(owner, days_ago, shares=1000, price=10.0):
    return {
        "owner": owner,
        "title": "CFO",
        "date": datetime.now(tz=timezone.utc) - timedelta(days=days_ago),
        "shares": shares,
        "price": price,
        "value": shares * price,
    }


class TestFindClusterRecency(unittest.TestCase):
    def test_recent_cluster_still_detected(self):
        buys = [_buy("Alice", 1), _buy("Bob", 1.5)]
        cluster = ic._find_cluster(buys)
        self.assertTrue(cluster, "a genuinely recent cluster must still be detected")
        self.assertEqual(len(cluster["insiders"]), 2)

    def test_years_old_cluster_is_discarded(self):
        # Both buys land within WINDOW_HOURS of each other, but the cluster
        # itself is ancient — this is the exact H6 defect.
        buys = [_buy("Alice", 900), _buy("Bob", 900.5)]
        cluster = ic._find_cluster(buys)
        self.assertEqual(cluster, {}, "a stale cluster must not re-emit as current conviction")

    def test_mixed_recent_and_stale_buys_only_clusters_recent_ones(self):
        buys = [_buy("Alice", 900), _buy("Bob", 900.5), _buy("Carol", 1), _buy("Dave", 1.2)]
        cluster = ic._find_cluster(buys)
        self.assertTrue(cluster)
        self.assertEqual(set(cluster["insiders"]), {"Carol", "Dave"})

    def test_max_age_env_knob_respected(self):
        old_max_age = ic.MAX_AGE_DAYS
        try:
            ic.MAX_AGE_DAYS = 3
            buys = [_buy("Alice", 5), _buy("Bob", 5.2)]
            self.assertEqual(ic._find_cluster(buys), {}, "buys older than a tightened MAX_AGE_DAYS are excluded")
        finally:
            ic.MAX_AGE_DAYS = old_max_age


if __name__ == "__main__":
    unittest.main()
