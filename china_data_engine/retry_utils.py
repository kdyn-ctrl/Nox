"""
Shared HTTP retry helper — exponential backoff + jitter for transient failures.

Every network fetch in this service should route through fetch_with_retry()
(direct `requests` calls) or call_with_retry() (any other callable, e.g. a
third-party client library) instead of a bare single-shot call, so a dropped
connection, timeout, or 429/5xx doesn't silently degrade to "no data" on the
very first hiccup. On final exhaustion both helpers log exactly which source
failed and why, so failures are traceable to a specific ticker/feed rather
than a generic "something went wrong".
"""

import random
import threading
import time
from typing import Any, Callable, Dict, Optional

import requests

# Statuses worth retrying: rate-limited or transient server-side failure.
# Anything else (401/403/404/etc.) is a caller/config problem — retrying
# won't help, so those are returned immediately for the caller to handle.
RETRYABLE_STATUSES = {429, 500, 502, 503, 504}


def fetch_with_retry(
    url: str,
    *,
    source: str,
    headers: Optional[Dict[str, str]] = None,
    params: Optional[Dict[str, Any]] = None,
    timeout=(5, 10),
    max_attempts: int = 3,
    backoff_base: float = 1.5,
) -> Optional[requests.Response]:
    """
    GET `url` with exponential backoff + jitter on connection errors, timeouts,
    and retryable HTTP statuses (429/5xx). `source` identifies exactly what is
    being fetched (e.g. "SEC Form4:AAPL", "Alpaca news") so a failure names
    the specific data that is missing.

    Returns the Response on success — either 2xx or a non-retryable status,
    left for the caller to interpret (e.g. a 404 should not be retried but
    the caller still needs to see it) — or None once every attempt against a
    retryable failure is exhausted.
    """
    last_error = "unknown error"
    for attempt in range(1, max_attempts + 1):
        try:
            resp = requests.get(url, headers=headers, params=params, timeout=timeout)
        except requests.RequestException as e:
            last_error = str(e)
        else:
            if resp.status_code not in RETRYABLE_STATUSES:
                return resp
            last_error = f"HTTP {resp.status_code}"
            retry_after = resp.headers.get("Retry-After")
            if retry_after is not None and attempt < max_attempts:
                try:
                    time.sleep(min(float(retry_after), 30.0))
                    continue
                except ValueError:
                    pass

        if attempt < max_attempts:
            sleep_s = backoff_base * (2 ** (attempt - 1)) + random.uniform(0, 0.5)
            print(
                f"[WARN] [RETRY] {source}: attempt {attempt}/{max_attempts} failed "
                f"({last_error}); retrying in {sleep_s:.1f}s",
                flush=True,
            )
            time.sleep(sleep_s)

    print(f"[ERROR] [RETRY] {source}: all {max_attempts} attempts failed — {last_error}", flush=True)
    return None


def _run_with_hard_timeout(fn: Callable[[], Any], timeout_seconds: float) -> Any:
    """
    Runs `fn()` in a daemon thread and hard-kills the wait after
    `timeout_seconds`. Some third-party SDKs (e.g. AkShare) expose no timeout
    parameter of their own and can hang indefinitely on a stalled upstream
    connection — this bounds a single call's wall-clock time regardless.

    Raises TimeoutError if the call is still running when the deadline hits
    (the underlying thread is abandoned as a daemon and will not block
    process exit). Re-raises any exception the callable itself raised.
    """
    result: list = [None]
    exc: list = [None]

    def _run():
        try:
            result[0] = fn()
        except Exception as e:  # noqa: BLE001 — re-raised on the calling thread below
            exc[0] = e

    t = threading.Thread(target=_run, daemon=True)
    t.start()
    t.join(timeout=timeout_seconds)
    if t.is_alive():
        raise TimeoutError(f"call timed out after {timeout_seconds}s")
    if exc[0] is not None:
        raise exc[0]
    return result[0]


def call_with_retry(
    fn: Callable[[], Any],
    *,
    source: str,
    max_attempts: int = 3,
    backoff_base: float = 1.5,
    is_failure: Optional[Callable[[Any], bool]] = None,
    timeout_seconds: Optional[float] = None,
) -> Optional[Any]:
    """
    Retry any zero-arg callable with the same backoff policy as
    fetch_with_retry, for sources that don't go through `requests` directly
    (e.g. a third-party SDK that wraps its own HTTP client).

    `is_failure` is an optional predicate on the return value: if it returns
    True, the result is treated as a retryable failure even though `fn()`
    didn't raise (e.g. a library that returns an empty dataframe instead of
    raising on a transient upstream error).

    `timeout_seconds`, if given, hard-bounds each individual attempt's
    wall-clock time via a daemon-thread watchdog (see _run_with_hard_timeout)
    — a hung call (e.g. AkShare, which exposes no timeout of its own) is
    treated as a retryable failure and does not block the retry loop, or the
    caller's scheduler, indefinitely.

    Returns fn()'s result, or None once every attempt is exhausted.
    """
    last_error = "unknown error"
    for attempt in range(1, max_attempts + 1):
        try:
            if timeout_seconds is not None:
                result = _run_with_hard_timeout(fn, timeout_seconds)
            else:
                result = fn()
            if is_failure is None or not is_failure(result):
                return result
            last_error = "empty/invalid result"
        except Exception as e:
            last_error = str(e)

        if attempt < max_attempts:
            sleep_s = backoff_base * (2 ** (attempt - 1)) + random.uniform(0, 0.5)
            print(
                f"[WARN] [RETRY] {source}: attempt {attempt}/{max_attempts} failed "
                f"({last_error}); retrying in {sleep_s:.1f}s",
                flush=True,
            )
            time.sleep(sleep_s)

    print(f"[ERROR] [RETRY] {source}: all {max_attempts} attempts failed — {last_error}", flush=True)
    return None
