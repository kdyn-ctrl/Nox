"""Unit tests for _strip_ixbrl_viewer (SEC inline-XBRL viewer-URL fix).

EDGAR lists a filing's primary document as the inline-XBRL VIEWER URL
(…/ix?doc=/Archives/…htm), which serves a JavaScript-only shell instead of the
filing. The resolver must strip that wrapper so the raw document is fetched.

Imported in isolation via importlib so we don't trigger monitor.py's heavy
import-time side effects (env validation, telebot/anthropic clients).
"""
import importlib.util
import os
import sys
import types


def _load_strip_fn():
    # Stub the heavy/third-party deps monitor.py imports at module load, and set
    # the env its require_env() checks, so we can import it here to reach the
    # pure helper without a live bot/anthropic/network.
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
    return monitor._strip_ixbrl_viewer


strip = _load_strip_fn()


class TestStripIxbrlViewer:
    def test_strips_relative_viewer_url(self):
        href = "/ix?doc=/Archives/edgar/data/320193/000032019326000011/aapl-20260430.htm"
        assert strip(href) == \
            "https://www.sec.gov/Archives/edgar/data/320193/000032019326000011/aapl-20260430.htm"

    def test_strips_absolute_viewer_url(self):
        href = "https://www.sec.gov/ix?doc=/Archives/edgar/data/1/x.htm"
        assert strip(href) == "https://www.sec.gov/Archives/edgar/data/1/x.htm"

    def test_plain_relative_href_is_made_absolute(self):
        href = "/Archives/edgar/data/1/x.htm"
        assert strip(href) == "https://www.sec.gov/Archives/edgar/data/1/x.htm"

    def test_plain_absolute_href_unchanged(self):
        href = "https://www.sec.gov/Archives/edgar/data/1/x.htm"
        assert strip(href) == href

    def test_result_never_contains_the_viewer_prefix(self):
        for href in (
            "/ix?doc=/Archives/edgar/data/1/x.htm",
            "https://www.sec.gov/ix?doc=/Archives/edgar/data/1/x.htm",
        ):
            assert "ix?doc=" not in strip(href)
