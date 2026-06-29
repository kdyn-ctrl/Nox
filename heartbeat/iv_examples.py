#!/usr/bin/env python3
"""
Example usage patterns for the Historical IV System.
Run these after the heartbeat service has collected at least a few days of data.
"""

# ============================================================================
# EXAMPLE 1: Get IV Rank for a single ticker (full calculation)
# ============================================================================
def example_single_ticker_iv_rank():
    """Query IV Rank for NVDA with automatic IV fetch from Alpaca."""
    from heartbeat.monitor import calculate_iv_rank

    result = calculate_iv_rank("NVDA")

    if result['method'] == 'full_history':
        print(f"✓ NVDA IV Rank: {result['iv_rank']:.2%}")
        print(f"  Current IV: {result['current_iv']:.4f}")
        print(f"  52-week range: {result['iv_min']:.4f} – {result['iv_max']:.4f}")
        print(f"  Based on {result['data_points']} snapshots over {result['days_available']} trading days")
    elif result['method'] == 'snapshot_relative':
        print(f"⚠ NVDA: Limited data ({result['days_available']} days available, need 30)")
        print(f"  Using snapshot-relative IV Rank: {result['iv_rank']:.2%}")
        print(f"  Average historical IV: {result['average_iv']:.4f}")
    elif result['method'] == 'error':
        print(f"✗ NVDA: {result['error']}")


# ============================================================================
# EXAMPLE 2: Batch IV Rank for entire watchlist
# ============================================================================
def example_batch_iv_rank():
    """Query IV Rank for all watchlist tickers and rank them."""
    from heartbeat.monitor import calculate_iv_rank, WATCHLIST

    results = {}
    for ticker in WATCHLIST:
        results[ticker] = calculate_iv_rank(ticker)

    # Sort by IV Rank (highest to lowest — high IV = extended volatility)
    ranked = sorted(
        [(t, r) for t, r in results.items() if r['iv_rank'] is not None],
        key=lambda x: x[1]['iv_rank'],
        reverse=True
    )

    print("IV Rank Ranking (Highest IV Extension First)")
    print("─" * 50)
    for rank, (ticker, result) in enumerate(ranked, 1):
        method = "✓" if result['method'] == 'full_history' else "⚠"
        print(f"{rank:2}. {ticker:6} {result['iv_rank']:6.2%}  {method} ({result['days_available']} days)")

    # Show failures
    errors = [(t, r) for t, r in results.items() if r['method'] == 'error']
    if errors:
        print("\nErrors:")
        for ticker, result in errors:
            print(f"  {ticker}: {result['error']}")


# ============================================================================
# EXAMPLE 3: Compare current IV to historical percentiles
# ============================================================================
def example_iv_level_context():
    """Show whether today's IV is high, normal, or low historically."""
    from heartbeat.monitor import calculate_iv_rank

    ticker = "TSLA"
    result = calculate_iv_rank(ticker)

    if result['method'] == 'full_history':
        rank = result['iv_rank']
        current = result['current_iv']

        if rank >= 0.75:
            context = "🔴 ELEVATED (75th+ percentile) — extended vol environment"
        elif rank >= 0.50:
            context = "🟡 NORMAL (50-75th percentile) — typical vol range"
        elif rank >= 0.25:
            context = "🟢 COMPRESSED (25-50th percentile) — low vol environment"
        else:
            context = "🔵 MINIMAL (<25th percentile) — historically tight range"

        print(f"{ticker} IV Analysis:")
        print(f"  Current IV: {current:.2%}")
        print(f"  Rank: {rank:.2%}")
        print(f"  {context}")
        print(f"  52-week range: {result['iv_min']:.2%} – {result['iv_max']:.2%}")


# ============================================================================
# EXAMPLE 4: Build IV percentile heatmap (for monitoring)
# ============================================================================
def example_iv_heatmap():
    """Visual heatmap of IV Rank across watchlist (for Telegram alerts)."""
    from heartbeat.monitor import calculate_iv_rank, WATCHLIST

    print("IV Rank Heatmap")
    print("=" * 60)

    for ticker in WATCHLIST:
        result = calculate_iv_rank(ticker)

        if result['method'] == 'error':
            bar = "✗ ERROR"
        elif result['iv_rank'] is None:
            bar = "? UNKNOWN"
        else:
            rank = result['iv_rank']
            # Create visual bar (0% = all spaces, 100% = all solid)
            filled = int(rank * 20)
            bar = "█" * filled + "░" * (20 - filled)

        print(f"{ticker:6} {bar:24} {result.get('iv_rank', '?'):.1%}")


# ============================================================================
# EXAMPLE 5: Detect IV divergence (cross-market signal)
# ============================================================================
def example_iv_divergence():
    """Detect when one stock's IV is abnormal vs peers (breadth signal)."""
    from heartbeat.monitor import calculate_iv_rank, DOMESTIC_WATCHLIST
    import statistics

    # Get IV Rank for domestic tech stocks
    ranks = {}
    for ticker in DOMESTIC_WATCHLIST:
        result = calculate_iv_rank(ticker)
        if result['iv_rank'] is not None:
            ranks[ticker] = result['iv_rank']

    if len(ranks) < 2:
        print("Not enough data for divergence analysis")
        return

    # Calculate group statistics
    mean_rank = statistics.mean(ranks.values())
    stdev_rank = statistics.stdev(ranks.values()) if len(ranks) > 1 else 0

    print("IV Divergence Analysis (Domestic Watchlist)")
    print("─" * 60)
    print(f"Group Mean Rank: {mean_rank:.2%}")
    print(f"Std Dev: {stdev_rank:.2%}")
    print()

    outliers = []
    for ticker, rank in sorted(ranks.items(), key=lambda x: x[1], reverse=True):
        zscore = (rank - mean_rank) / stdev_rank if stdev_rank > 0 else 0
        divergence = "⬆ HIGH" if rank > mean_rank else "⬇ LOW"

        if abs(zscore) > 1.5:
            outliers.append(ticker)
            print(f"🚨 {ticker:6} {rank:.2%} (z={zscore:+.2f}) — {divergence} [OUTLIER]")
        else:
            print(f"   {ticker:6} {rank:.2%} (z={zscore:+.2f}) — {divergence}")

    if outliers:
        print(f"\nOutliers: {', '.join(outliers)}")
        print("→ Potential cross-market basis trade signal")


# ============================================================================
# EXAMPLE 6: Historical IV trend (rolling metric)
# ============================================================================
def example_iv_trend():
    """Show IV trend: is volatility expanding or contracting?"""
    import sqlite3
    from heartbeat.monitor import DB_PATH, db_lock
    from datetime import datetime, timedelta

    ticker = "AAPL"
    lookback_days = 10

    with db_lock:
        with sqlite3.connect(DB_PATH) as conn:
            c = conn.cursor()
            c.execute(
                """
                SELECT implied_volatility, date
                FROM historical_volatility
                WHERE ticker = ?
                ORDER BY date DESC
                LIMIT ?
                """,
                (ticker, lookback_days)
            )
            rows = c.fetchall()

    if len(rows) < 2:
        print(f"Not enough data for {ticker} trend")
        return

    ivs = [row[0] for row in reversed(rows)]  # Reverse to chronological order
    dates = [row[1] for row in reversed(rows)]

    print(f"{ticker} IV Trend (Last {len(ivs)} days)")
    print("─" * 40)

    for date, iv in zip(dates, ivs):
        bar_len = int(iv * 100)  # Scale to percentage
        bar = "█" * bar_len
        print(f"{date} {iv:.4f}  {bar}")

    # Calculate trend
    first_iv = ivs[0]
    last_iv = ivs[-1]
    change = (last_iv - first_iv) / first_iv * 100

    if change > 5:
        trend = "⬆ EXPANDING volatility"
    elif change < -5:
        trend = "⬇ CONTRACTING volatility"
    else:
        trend = "➜ STABLE volatility"

    print(f"\nTrend: {change:+.1f}% → {trend}")


# ============================================================================
# EXAMPLE 7: IV skew (WS1 Contradiction Vector input)
# ============================================================================
def example_iv_skew():
    """
    Compute put-vs-call IV skew for a ticker. Positive skew = puts bid up =
    bearish options positioning; the Contradiction Vector flags bullish text
    against a bearish skew as a signal to IGNORE.
    """
    from heartbeat.monitor import fetch_iv_skew

    ticker = "NVDA"
    result = fetch_iv_skew(ticker)

    if result.get("method") != "live_chain":
        print(f"✗ {ticker}: {result.get('error', 'no data')}")
        return

    skew = result["skew"]
    direction = (
        "🔴 BEARISH (puts bid up)" if result["skew_pct"] >= 0.03
        else "🟢 BULLISH (calls bid up)" if result["skew_pct"] <= -0.03
        else "⚪ NEUTRAL"
    )
    print(f"{ticker} IV Skew:")
    print(f"  Call IV: {result['call_iv']:.2%}   Put IV: {result['put_iv']:.2%}")
    print(f"  Skew: {skew:+.4f} ({result['skew_pct']:+.1%})  {direction}")
    print(f"  Put/Call OI ratio: {result['put_call_oi_ratio']:.2f}")


if __name__ == "__main__":
    import sys

    examples = {
        "1": ("Single ticker IV Rank", example_single_ticker_iv_rank),
        "2": ("Batch IV Rank (all watchlist)", example_batch_iv_rank),
        "3": ("IV level context", example_iv_level_context),
        "4": ("IV Rank heatmap", example_iv_heatmap),
        "5": ("IV divergence detection", example_iv_divergence),
        "6": ("IV trend analysis", example_iv_trend),
        "7": ("IV skew (Contradiction Vector)", example_iv_skew),
    }

    if len(sys.argv) > 1:
        example_num = sys.argv[1]
        if example_num in examples:
            print(f"\n{examples[example_num][0]}")
            print("=" * 60)
            examples[example_num][1]()
        else:
            print(f"Unknown example: {example_num}")
            print(f"Available: {', '.join(examples.keys())}")
    else:
        print("Historical IV System — Usage Examples")
        print("=" * 60)
        print("\nRun: python3 iv_examples.py <number>\n")
        for num, (desc, _) in examples.items():
            print(f"  {num}: {desc}")
