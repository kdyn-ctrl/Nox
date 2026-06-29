#!/usr/bin/env python3
"""
Weekend Research Batch — Skeptic Report Generator (WS6)

Runs inside the america-data-engine container on Saturday ~04:00 ET.
Calls each Skeptic workstream directly (no HTTP) to gather fresh data,
then writes a machine-readable JSON report and a human-readable Markdown
summary for Sunday-morning review before futures open.

Output (written to /app/data/):
    skeptic_report_YYYY-MM-DD.json
    skeptic_report_YYYY-MM-DD.md

Exit codes:
    0 — report written successfully
    1 — one or more pipeline stages failed (partial report still written)
"""

import json
import math
import os
import sys
import traceback
from datetime import datetime, timezone
from pathlib import Path

from scrapers import fetch_alpaca_news, fetch_earnings_calendar
from contradiction_vector import run_contradiction_check
from insider_cluster import detect_insider_clusters
from alt_macro import run_alt_macro_check

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
WATCHLIST: list[str] = [
    "AAPL", "TSLA", "NVDA", "MSFT", "BABA", "JD", "PDD", "BIDU", "NIO",
]
OUTPUT_DIR = Path(os.getenv("SKEPTIC_REPORT_DIR", "/app/data"))
OUTPUT_DIR.mkdir(parents=True, exist_ok=True)


# ---------------------------------------------------------------------------
# Pipeline stages — each returns (key, result, error_or_None)
# ---------------------------------------------------------------------------

def _run_contradiction() -> tuple[str, dict, str | None]:
    try:
        print("[WS6] Fetching news for contradiction check...", flush=True)
        news = fetch_alpaca_news()
        print(f"[WS6] {len(news)} article(s) fetched.", flush=True)
        result = run_contradiction_check(news)
        print(f"[WS6] Contradiction check: {len(result.get('results', []))} ticker(s) evaluated.", flush=True)
        return "contradiction", result, None
    except Exception:
        return "contradiction", {}, traceback.format_exc()


def _run_insider_clusters() -> tuple[str, dict, str | None]:
    try:
        print("[WS6] Scanning SEC Form 4 filings for insider clusters...", flush=True)
        result = detect_insider_clusters(WATCHLIST)
        n = len(result.get("signals", []))
        print(f"[WS6] Insider cluster scan: {n} cluster signal(s) found.", flush=True)
        return "insider_clusters", result, None
    except Exception:
        return "insider_clusters", {}, traceback.format_exc()


def _run_alt_macro() -> tuple[str, dict, str | None]:
    try:
        print("[WS6] Running alternative-macro check...", flush=True)
        result = run_alt_macro_check()
        n = len(result.get("regions", []))
        print(f"[WS6] Alt-macro check: {n} region(s) evaluated.", flush=True)
        return "alt_macro", result, None
    except Exception:
        return "alt_macro", {}, traceback.format_exc()


def _run_earnings() -> tuple[str, dict, str | None]:
    try:
        print("[WS6] Fetching 30-day earnings calendar...", flush=True)
        result = fetch_earnings_calendar(WATCHLIST)
        total = sum(len(v) for v in result.values())
        print(f"[WS6] Earnings calendar: {total} event(s) found.", flush=True)
        return "earnings_calendar", result, None
    except Exception:
        return "earnings_calendar", {}, traceback.format_exc()


# ---------------------------------------------------------------------------
# Signal quality scoring
# ---------------------------------------------------------------------------

def _score_contradiction(data: dict) -> tuple[str, list[str]]:
    """High conviction when a confirmed signal exists; flag contradictions."""
    results = data.get("results", [])
    confirmed = [r for r in results if r.get("verdict", "").startswith("CONFIRM")]
    contradicted = [r for r in results if r.get("verdict", "").startswith("CONTRADICT")]
    ignored = [r for r in results if r.get("verdict") == "IGNORE"]
    bullets = []
    if confirmed:
        bullets.append(f"CONFIRMED signals ({len(confirmed)}): " + ", ".join(r["ticker"] for r in confirmed))
    if contradicted:
        bullets.append(f"CONTRADICTIONS suppressed ({len(contradicted)}): " + ", ".join(r["ticker"] for r in contradicted))
    if ignored:
        bullets.append(f"Signals IGNORED — bullish text vs bearish IV ({len(ignored)}): " + ", ".join(r["ticker"] for r in ignored))
    if not results:
        bullets.append("No contradiction data available.")
    level = "HIGH" if confirmed else ("MEDIUM" if not contradicted else "LOW")
    return level, bullets


def _score_insider(data: dict) -> tuple[str, list[str]]:
    signals = data.get("signals", [])
    bullets = []
    for s in signals:
        ticker = s.get("ticker", "?")
        n = s.get("distinct_insiders", 0)
        roles = s.get("roles", [])
        bullets.append(f"{ticker}: {n} insider(s) — {', '.join(roles)}")
    if not signals:
        bullets.append("No insider cluster signals detected.")
    level = "HIGH" if len(signals) >= 2 else ("MEDIUM" if signals else "LOW")
    return level, bullets


def _score_alt_macro(data: dict) -> tuple[str, list[str]]:
    regions = data.get("regions", [])
    bullets = []
    contradictions = []
    for r in regions:
        verdict = r.get("verdict", "NO_DATA")
        region = r.get("region", "?")
        bias = r.get("commodity_bias", "")
        reason = r.get("reason", "")
        line = f"{region}: {verdict}"
        if bias:
            line += f" ({bias})"
        if reason:
            line += f" — {reason}"
        bullets.append(line)
        if verdict == "TEXT_CONTRADICTS_PHYSICAL":
            contradictions.append(region)
    if not regions:
        bullets.append("No alternative-macro data available.")
    level = "HIGH" if contradictions else ("MEDIUM" if regions else "LOW")
    return level, bullets


# ---------------------------------------------------------------------------
# Markdown renderer
# ---------------------------------------------------------------------------

def _render_markdown(report: dict) -> str:
    ts = report["generated_at"]
    errors = report.get("pipeline_errors", {})
    lines: list[str] = []

    lines.append(f"# Nox Skeptic Report — {ts[:10]}")
    lines.append(f"")
    lines.append(f"*Generated: {ts} UTC*")
    lines.append(f"*Workstreams: WS1 (Contradiction) · WS2 (Alt Macro) · WS3 (Insider) · WS4 (Decay) · WS5 (Liquidity)*")
    lines.append(f"")

    # Overall conviction
    levels = [report["summary"][k]["conviction"] for k in ("contradiction", "insider", "alt_macro")]
    high = levels.count("HIGH")
    med = levels.count("MEDIUM")
    overall = "HIGH" if high >= 2 else ("MEDIUM" if high + med >= 2 else "LOW")
    lines.append(f"## Overall Signal Quality: {overall}")
    lines.append(f"")

    # WS1 — Contradiction Vector
    cv = report["summary"]["contradiction"]
    lines.append(f"### WS1 — Contradiction Vector: {cv['conviction']}")
    for b in cv["bullets"]:
        lines.append(f"- {b}")
    if "contradiction" in errors:
        lines.append(f"- **ERROR**: pipeline failed — see `pipeline_errors.contradiction`")
    lines.append(f"")

    # WS3 — Insider Clusters
    ins = report["summary"]["insider"]
    lines.append(f"### WS3 — Insider Cluster Filter: {ins['conviction']}")
    for b in ins["bullets"]:
        lines.append(f"- {b}")
    if "insider_clusters" in errors:
        lines.append(f"- **ERROR**: pipeline failed — see `pipeline_errors.insider_clusters`")
    lines.append(f"")

    # WS2 — Alt Macro
    am = report["summary"]["alt_macro"]
    lines.append(f"### WS2 — Alternative Macro: {am['conviction']}")
    for b in am["bullets"]:
        lines.append(f"- {b}")
    if "alt_macro" in errors:
        lines.append(f"- **ERROR**: pipeline failed — see `pipeline_errors.alt_macro`")
    lines.append(f"")

    # Earnings calendar
    lines.append(f"### Earnings Calendar (next 30 days)")
    ec = report.get("earnings_calendar", {})
    if ec:
        for ticker, events in sorted(ec.items()):
            for ev in events:
                lines.append(f"- **{ticker}** — {ev.get('date', '?')}: {ev.get('description', 'Earnings')}")
    else:
        lines.append(f"- No upcoming earnings found.")
    lines.append(f"")

    # Decay half-lives reminder
    lines.append(f"### Active Decay Configuration (WS4)")
    lines.append(f"- GEOPOLITICAL: {os.getenv('HALFLIFE_GEOPOLITICAL_HOURS', '6')}h half-life")
    lines.append(f"- MACRO: {os.getenv('HALFLIFE_MACRO_HOURS', '48')}h half-life")
    lines.append(f"- EARNINGS: {os.getenv('HALFLIFE_EARNINGS_HOURS', '72')}h half-life")
    lines.append(f"- TECHNICAL: {os.getenv('HALFLIFE_TECHNICAL_HOURS', '12')}h half-life")
    bypass_flags = []
    for flag in ("CONTRADICTION_BYPASS", "INSIDER_CLUSTER_BYPASS", "ALT_MACRO_BYPASS",
                 "HALFLIFE_DECAY_BYPASS", "REGIME_RESET_BYPASS", "LIQUIDITY_GATE_BYPASS"):
        if os.getenv(flag, "false").lower() == "true":
            bypass_flags.append(flag)
    if bypass_flags:
        lines.append(f"")
        lines.append(f"> **WARNING — Bypass flags active (backtesting mode):** {', '.join(bypass_flags)}")
    lines.append(f"")

    lines.append(f"---")
    lines.append(f"*Review before futures open Sunday. All gates must pass before live execution.*")
    lines.append(f"")

    return "\n".join(lines)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main() -> int:
    now_utc = datetime.now(tz=timezone.utc)
    date_str = now_utc.strftime("%Y-%m-%d")
    ts_str = now_utc.isoformat(timespec="seconds")

    print(f"[WS6] Skeptic Report generation starting — {ts_str}", flush=True)

    # Run all pipeline stages
    stages = [_run_contradiction, _run_insider_clusters, _run_alt_macro, _run_earnings]
    pipeline_data: dict = {}
    pipeline_errors: dict = {}

    for fn in stages:
        key, result, err = fn()
        pipeline_data[key] = result
        if err:
            pipeline_errors[key] = err
            print(f"[WS6] [WARN] Stage '{key}' failed:\n{err}", flush=True)

    # Score conviction
    c_level, c_bullets = _score_contradiction(pipeline_data.get("contradiction", {}))
    i_level, i_bullets = _score_insider(pipeline_data.get("insider_clusters", {}))
    a_level, a_bullets = _score_alt_macro(pipeline_data.get("alt_macro", {}))

    report = {
        "generated_at": ts_str,
        "report_date":  date_str,
        "summary": {
            "contradiction": {"conviction": c_level, "bullets": c_bullets},
            "insider":        {"conviction": i_level, "bullets": i_bullets},
            "alt_macro":      {"conviction": a_level, "bullets": a_bullets},
        },
        "earnings_calendar": pipeline_data.get("earnings_calendar", {}),
        "raw": {
            "contradiction":    pipeline_data.get("contradiction", {}),
            "insider_clusters": pipeline_data.get("insider_clusters", {}),
            "alt_macro":        pipeline_data.get("alt_macro", {}),
        },
        "pipeline_errors": pipeline_errors,
        "bypass_flags": {
            flag: os.getenv(flag, "false")
            for flag in (
                "CONTRADICTION_BYPASS", "INSIDER_CLUSTER_BYPASS", "ALT_MACRO_BYPASS",
                "HALFLIFE_DECAY_BYPASS", "REGIME_RESET_BYPASS", "LIQUIDITY_GATE_BYPASS",
            )
        },
    }

    # Write JSON
    json_path = OUTPUT_DIR / f"skeptic_report_{date_str}.json"
    json_path.write_text(json.dumps(report, indent=2, default=str))
    print(f"[WS6] JSON report written: {json_path}", flush=True)

    # Write Markdown
    md_path = OUTPUT_DIR / f"skeptic_report_{date_str}.md"
    md_path.write_text(_render_markdown(report))
    print(f"[WS6] Markdown report written: {md_path}", flush=True)

    if pipeline_errors:
        print(f"[WS6] Completed with {len(pipeline_errors)} stage error(s). Report is partial.", flush=True)
        return 1

    print(f"[WS6] Report generation complete. Review before Sunday futures open.", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
