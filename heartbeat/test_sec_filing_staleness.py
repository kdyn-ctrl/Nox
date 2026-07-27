"""Unit tests for get_latest_sec_filing()'s staleness gate.

2026-07-19: the function unconditionally surfaced the newest entry in a
ticker's 8-K/6-K Atom feed with no check of how old it actually was — 8-Ks
are event-driven, not periodic, so a quiet ticker's "latest" filing can be
months old. Confirmed live: AAPL's newest 8-K on record was dated 2026-04-30
and the report presented it undated, as if it were current SEC news.

Imported in isolation via importlib so we don't trigger monitor.py's heavy
import-time side effects (env validation, telebot/anthropic clients).
"""
import importlib.util
import os
import sys
import types
from unittest.mock import MagicMock, patch


def _load_monitor():
    for name in ("telebot", "telebot.util", "anthropic", "schedule", "requests",
                 "pandas", "pytrends", "pytrends.request", "akshare"):
        sys.modules.setdefault(name, types.ModuleType(name))
    sys.modules["telebot"].TeleBot = lambda *a, **k: types.SimpleNamespace(
        message_handler=lambda *a, **k: (lambda f: f),
        send_message=lambda *a, **k: None, reply_to=lambda *a, **k: None)
    sys.modules["telebot"].util = sys.modules["telebot.util"]
    sys.modules["telebot.util"].smart_split = lambda *a, **k: []
    sys.modules["anthropic"].Anthropic = lambda *a, **k: None
    sys.modules["schedule"].clear = lambda *a, **k: None
    sys.modules["pytrends.request"].TrendReq = object
    r = sys.modules["requests"]
    r.Response = type("Response", (), {})
    r.get = r.post = lambda *a, **k: None
    r.exceptions = types.SimpleNamespace(RequestException=Exception, Timeout=Exception)
    for k, v in {"TELEGRAM_BOT_TOKEN": "x", "TELEGRAM_CHAT_ID": "1",
                 "ANTHROPIC_API_KEY": "x", "ALPACA_API_KEY": "x",
                 "ALPACA_SECRET_KEY": "x", "WEBHOOK_SECRET_TOKEN": "x"}.items():
        os.environ.setdefault(k, v)
    import monitor
    return monitor


monitor = _load_monitor()


def _atom_feed(updated_iso: str) -> bytes:
    return f"""<?xml version="1.0"?>
<feed xmlns="http://www.w3.org/2005/Atom">
  <entry>
    <link href="https://www.sec.gov/Archives/edgar/data/1/x-index.htm" rel="alternate" type="text/html" />
    <updated>{updated_iso}</updated>
  </entry>
</feed>""".encode()


def _resp(content: bytes, status_code: int = 200):
    m = MagicMock()
    m.status_code = status_code
    m.content = content
    return m


class TestSecFilingStaleness:
    def test_filing_older_than_max_age_is_not_surfaced(self):
        # AAPL's real 2026-04-30 8-K, evaluated as of ~2026-07-20 — ~80 days old.
        feed = _resp(_atom_feed("2026-04-30T16:30:41-04:00"))
        with patch("monitor.fetch_with_retry", return_value=feed), \
             patch("monitor.get_filing_type", return_value="8-K"):
            text, ok = monitor.get_latest_sec_filing("AAPL")
        assert ok is True
        assert "stale" in text.lower()
        assert "2026-04-30" in text

    def test_filing_within_max_age_is_surfaced_with_its_date(self):
        from datetime import datetime, timedelta, timezone
        recent = (datetime.now(timezone.utc) - timedelta(days=2)).isoformat()
        feed = _resp(_atom_feed(recent))
        doc = _resp(b"", status_code=200)
        doc.text = "<html><body>Filing body text.</body></html>"
        with patch("monitor.fetch_with_retry", side_effect=[feed, doc]), \
             patch("monitor.get_filing_type", return_value="8-K"), \
             patch("monitor.resolve_primary_document", return_value="https://www.sec.gov/x.htm"):
            text, ok = monitor.get_latest_sec_filing("AAPL")
        assert ok is True
        assert "Filing body text." in text
        assert "filed " in text  # date-anchored, not presented as undated/current

    def test_unparseable_updated_date_falls_through_without_blocking(self):
        feed = _resp(_atom_feed("not-a-date"))
        doc = _resp(b"", status_code=200)
        doc.text = "<html><body>Filing body text.</body></html>"
        with patch("monitor.fetch_with_retry", side_effect=[feed, doc]), \
             patch("monitor.get_filing_type", return_value="8-K"), \
             patch("monitor.resolve_primary_document", return_value="https://www.sec.gov/x.htm"):
            text, ok = monitor.get_latest_sec_filing("AAPL")
        assert ok is True
        assert "Filing body text." in text
