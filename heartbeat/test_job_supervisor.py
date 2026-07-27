"""Unit tests for job_supervisor.py (scheduled-job dead-man's switch, RULE-D3).

Pure-logic tests — no telebot/anthropic/DB; the alert sink is an injected list.
Run: python3 -m pytest test_job_supervisor.py
"""
import job_supervisor
from job_supervisor import JobSupervisor


def _sup(threshold=2):
    """A supervisor whose alerts append to a captured list."""
    alerts = []
    sup = JobSupervisor(alert_fn=alerts.append, threshold=threshold)
    return sup, alerts


# ── record(): failure-streak alarm ─────────────────────────────────────────
class TestFailureAlarm:
    def test_no_alert_before_threshold(self):
        sup, alerts = _sup(threshold=2)
        sup.record("job", ok=False, detail="boom")
        assert alerts == []  # 1 failure < threshold 2

    def test_alert_fires_once_at_threshold(self):
        sup, alerts = _sup(threshold=2)
        sup.record("job", ok=False)
        sup.record("job", ok=False)  # 2nd consecutive → alarm
        assert len(alerts) == 1
        assert "FAILING" in alerts[0] and "job" in alerts[0]

    def test_alert_does_not_repeat_past_threshold(self):
        sup, alerts = _sup(threshold=2)
        for _ in range(5):
            sup.record("job", ok=False)
        assert len(alerts) == 1  # deduped — fires only on the threshold edge

    def test_last_error_detail_is_included(self):
        sup, alerts = _sup(threshold=1)
        sup.record("job", ok=False, detail="creds expired")
        assert "creds expired" in alerts[0]


# ── record(): recovery re-arm ──────────────────────────────────────────────
class TestRecovery:
    def test_success_after_alarm_fires_recovery(self):
        sup, alerts = _sup(threshold=2)
        sup.record("job", ok=False)
        sup.record("job", ok=False)  # alarm
        sup.record("job", ok=True)   # recovery
        assert len(alerts) == 2
        assert "RECOVERED" in alerts[1]

    def test_success_without_prior_alarm_is_silent(self):
        sup, alerts = _sup(threshold=2)
        sup.record("job", ok=False)  # 1 failure, below threshold
        sup.record("job", ok=True)   # recovers before alarming
        assert alerts == []

    def test_re_arm_allows_a_second_alarm(self):
        sup, alerts = _sup(threshold=2)
        sup.record("job", ok=False); sup.record("job", ok=False)  # alarm 1
        sup.record("job", ok=True)                                # recovery
        sup.record("job", ok=False); sup.record("job", ok=False)  # alarm 2
        kinds = [("FAILING" in a, "RECOVERED" in a) for a in alerts]
        assert kinds == [(True, False), (False, True), (True, False)]


# ── record(): jobs are independent ─────────────────────────────────────────
class TestPerJobIsolation:
    def test_streaks_do_not_bleed_across_jobs(self):
        sup, alerts = _sup(threshold=2)
        sup.record("a", ok=False)
        sup.record("b", ok=False)  # each at 1, neither alarms
        assert alerts == []
        sup.record("a", ok=False)  # a hits 2
        assert len(alerts) == 1 and "'a'" in alerts[0]


# ── record(): an alert-sink failure never propagates ───────────────────────
class TestAlertFailureIsSwallowed:
    def test_alert_exception_does_not_raise(self):
        def boom(_msg):
            raise RuntimeError("telegram down")
        sup = JobSupervisor(alert_fn=boom, threshold=1)
        # Must not raise even though the alert sink throws.
        sup.record("job", ok=False)


# ── supervise(): wrapper records outcomes and swallows ─────────────────────
class TestSupervise:
    def test_success_path_records_ok_and_returns_value(self):
        sup, alerts = _sup(threshold=1)
        wrapped = sup.supervise("job")(lambda: 42)
        assert wrapped() == 42
        assert alerts == []

    def test_exception_is_swallowed_and_recorded(self):
        sup, alerts = _sup(threshold=1)

        def raiser():
            raise ValueError("kaboom")

        wrapped = sup.supervise("job")(raiser)
        assert wrapped() is None      # swallowed — scheduler thread survives
        assert len(alerts) == 1
        assert "kaboom" in alerts[0]

    def test_success_after_failures_recovers(self):
        sup, alerts = _sup(threshold=1)
        state = {"fail": True}

        def flaky():
            if state["fail"]:
                raise RuntimeError("x")
            return "ok"

        w = sup.supervise("job")(flaky)
        w()                       # fail → alarm (threshold 1)
        state["fail"] = False
        assert w() == "ok"        # success → recovery
        assert any("FAILING" in a for a in alerts)
        assert any("RECOVERED" in a for a in alerts)

    def test_args_and_kwargs_pass_through(self):
        sup, _ = _sup()
        wrapped = sup.supervise("job")(lambda a, b=0: a + b)
        assert wrapped(3, b=4) == 7


# ── default threshold is env-tunable / present ─────────────────────────────
def test_default_threshold_exposed():
    assert isinstance(job_supervisor.DEFAULT_FAIL_ALERT_THRESHOLD, int)
    assert job_supervisor.DEFAULT_FAIL_ALERT_THRESHOLD >= 1
