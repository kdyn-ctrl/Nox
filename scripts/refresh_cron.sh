#!/bin/bash
# =============================================================================
# refresh_cron.sh — Install / remove the Nox data-refresh cron jobs
# =============================================================================
# Manages two crontab entries:
#
#   1. WEEKDAY DATA REFRESH (nox-data-refresh)
#      Calls refresh_data.sh every weekday at 21:00 ET.
#      Since cron runs in UTC, and ET is UTC-4 (EDT) or UTC-5 (EST),
#      we schedule at 01:00 UTC Tuesday–Saturday (≈ 21:00 ET Mon–Fri on
#      standard time).  During daylight saving this runs at 22:00 ET —
#      still well after the 20:00 ET market close.
#
#   2. WEEKEND SKEPTIC BATCH (nox-skeptic-weekend)  [WS6]
#      Calls weekend_batch.sh every Saturday at 08:00 UTC (≈ 04:00 ET EDT).
#      Runs WS1–3 skeptic pipelines and writes data/skeptic_report_*.{json,md}
#      for human-in-the-loop review before Sunday futures open.
#
# Both jobs are idempotent — --install replaces rather than duplicating.
#
# USAGE
#   ./refresh_cron.sh --install     add both cron jobs (idempotent)
#   ./refresh_cron.sh --remove      remove both cron jobs
#   ./refresh_cron.sh --status      show whether jobs are installed
#   ./refresh_cron.sh --help        print this help
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFRESH_SCRIPT="${SCRIPT_DIR}/refresh_data.sh"
WEEKEND_SCRIPT="${SCRIPT_DIR}/weekend_batch.sh"
REFRESH_LOG="${SCRIPT_DIR}/../logs/refresh.log"
WEEKEND_LOG="${SCRIPT_DIR}/../logs/weekend_batch.log"
CRON_MARKER="nox-data-refresh"
WEEKEND_MARKER="nox-skeptic-weekend"

# ---- Validate that both target scripts exist ------------------------------
if [ ! -f "$REFRESH_SCRIPT" ]; then
    echo "[CRON] ERROR: refresh_data.sh not found at: $REFRESH_SCRIPT"
    exit 1
fi
if [ ! -f "$WEEKEND_SCRIPT" ]; then
    echo "[CRON] ERROR: weekend_batch.sh not found at: $WEEKEND_SCRIPT"
    exit 1
fi

# Make sure both are executable
chmod +x "$REFRESH_SCRIPT"
chmod +x "$WEEKEND_SCRIPT"

# Weekday data refresh: 01:00 UTC Tue–Sat  →  21:00 ET Mon–Fri (standard time)
CRON_LINE="0 1 * * 2-6 ${REFRESH_SCRIPT} >> ${REFRESH_LOG} 2>&1   # ${CRON_MARKER}"

# Weekend Skeptic batch: 08:00 UTC Saturday  →  ~04:00 ET (EDT, UTC-4)
WEEKEND_LINE="0 8 * * 6 ${WEEKEND_SCRIPT} >> ${WEEKEND_LOG} 2>&1   # ${WEEKEND_MARKER}"

# ---------------------------------------------------------------------------
# Argument handling
# ---------------------------------------------------------------------------
ACTION="${1:-}"

case "$ACTION" in
    --install)
        echo "[CRON] Installing Nox cron jobs..."

        # Grab current crontab (ignore error if none exists yet)
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)

        # Remove any previous version of both managed lines
        CLEANED=$(echo "$CURRENT_CRON" | grep -v "$CRON_MARKER" | grep -v "$WEEKEND_MARKER" || true)

        # Append both lines
        {
            echo "$CLEANED"
            echo "$CRON_LINE"
            echo "$WEEKEND_LINE"
        } | crontab -

        echo "[CRON] Installed."
        echo ""
        echo "[CRON] Weekday data refresh:"
        echo "         $CRON_LINE"
        echo "[CRON] Weekend Skeptic batch (WS6):"
        echo "         $WEEKEND_LINE"
        echo ""
        echo "[CRON] To verify:  ./refresh_cron.sh --status"
        echo "[CRON] To remove:  ./refresh_cron.sh --remove"
        ;;

    --remove)
        echo "[CRON] Removing Nox cron jobs..."
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)
        REMOVED=0
        if echo "$CURRENT_CRON" | grep -q "$CRON_MARKER"; then
            REMOVED=1
        fi
        if echo "$CURRENT_CRON" | grep -q "$WEEKEND_MARKER"; then
            REMOVED=1
        fi
        if [ "$REMOVED" -eq 1 ]; then
            echo "$CURRENT_CRON" | grep -v "$CRON_MARKER" | grep -v "$WEEKEND_MARKER" | crontab -
            echo "[CRON] Removed."
        else
            echo "[CRON] No managed jobs found — nothing to remove."
        fi
        ;;

    --status)
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)
        echo "[CRON] Status:"
        if echo "$CURRENT_CRON" | grep -q "$CRON_MARKER"; then
            echo "  [INSTALLED] Weekday data refresh:"
            echo "    $(echo "$CURRENT_CRON" | grep "$CRON_MARKER")"
        else
            echo "  [MISSING]   Weekday data refresh (nox-data-refresh)"
        fi
        if echo "$CURRENT_CRON" | grep -q "$WEEKEND_MARKER"; then
            echo "  [INSTALLED] Weekend Skeptic batch (WS6):"
            echo "    $(echo "$CURRENT_CRON" | grep "$WEEKEND_MARKER")"
        else
            echo "  [MISSING]   Weekend Skeptic batch (nox-skeptic-weekend)"
        fi
        if ! echo "$CURRENT_CRON" | grep -q "$CRON_MARKER" || ! echo "$CURRENT_CRON" | grep -q "$WEEKEND_MARKER"; then
            echo ""
            echo "[CRON] Run './refresh_cron.sh --install' to add missing jobs."
        fi
        ;;

    --help|-h|"")
        sed -n '/^# ====/,/^# ====/p' "$0" | grep '^#' | sed 's/^# \?//'
        ;;

    *)
        echo "[CRON] Unknown argument: $ACTION"
        echo "       Usage: $0 {--install|--remove|--status|--help}"
        exit 1
        ;;
esac
