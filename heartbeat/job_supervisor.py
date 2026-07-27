"""Scheduled-job dead-man's switch (RULE-D3).

Every scheduled job in monitor.py historically only logged to stdout on
failure, so a job that failed on every run for weeks looked identical to
"nothing to report". This mirrors the C++ Skeptic "neutral N/M this scan" alarm
at job granularity: count every run, and when a job fails a threshold number of
times CONSECUTIVELY, fire ONE alert (deduped, re-armed on recovery — the same
shape as monitor.py's _liveness_supervisor). The MISSING success is the alarm.

Extracted into its own module so it is unit-testable without importing the whole
monitor.py (which has heavy import-time side effects: env validation, telebot /
anthropic client construction, sibling-module imports).
"""

import os
import functools
import threading

# Consecutive-failure count at which a job fires its alarm. Env-tunable
# (RULE-D11); fake-safe default of 2 = alert on the 2nd straight failure.
DEFAULT_FAIL_ALERT_THRESHOLD = int(os.getenv("JOB_FAIL_ALERT_THRESHOLD", "2"))


class JobSupervisor:
    """Tracks per-job consecutive-failure streaks and alerts on sustained
    failure. `alert_fn(message: str)` is injected so the module has no hard
    dependency on telebot; `logger` is optional."""

    def __init__(self, alert_fn, threshold=None, logger=None):
        self._alert = alert_fn
        self._threshold = threshold if threshold is not None else DEFAULT_FAIL_ALERT_THRESHOLD
        self._streak = {}
        self._lock = threading.Lock()
        self._logger = logger

    def record(self, name, ok, detail=""):
        """Record one job outcome; alert once when the streak first reaches the
        threshold, and once again on the first success after having alerted."""
        with self._lock:
            prev = self._streak.get(name, 0)
            if ok:
                self._streak[name] = 0
                recovered = prev >= self._threshold
                streak = prev
            else:
                streak = prev + 1
                self._streak[name] = streak
                recovered = False
        try:
            if ok and recovered:
                self._alert(
                    f"✅ RECOVERED: scheduled job '{name}' succeeded after "
                    f"{streak} consecutive failure(s).")
            elif not ok and streak == self._threshold:
                self._alert(
                    f"🚨 SCHEDULED JOB FAILING: '{name}' has failed {streak} "
                    f"consecutive run(s). Last error: {detail or 'n/a'}. The "
                    f"feed, credentials, or data source behind it is likely "
                    f"down — a silently-failing job looks identical to "
                    f"'nothing to report'.")
        except Exception as e:  # an alert failure must never break the job path
            print(f"[ERROR] [HEARTBEAT] job-result alert send failed for "
                  f"'{name}': {e}", flush=True)

    def supervise(self, name):
        """Decorator/wrapper: run a scheduled callable, record its outcome, and
        swallow exceptions so one job's failure never kills the scheduler
        thread. Apply at registration: `.do(sup.supervise("x")(fn))`.

        NOTE: a job that swallows its OWN exceptions internally returns normally
        and is recorded as a success — jobs whose total failure must be caught
        here have to let the exception propagate."""
        def deco(fn):
            @functools.wraps(fn)
            def wrapper(*args, **kwargs):
                try:
                    result = fn(*args, **kwargs)
                    self.record(name, True)
                    return result
                except Exception as e:
                    if self._logger:
                        self._logger.error(f"[{name}] scheduled job failed: {e}")
                    self.record(name, False, str(e))
                    return None
            return wrapper
        return deco
