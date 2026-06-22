#!/bin/bash
# =============================================================================
# refresh_cron.sh — Install / remove the Nox data-refresh cron job
# =============================================================================
# Installs a crontab entry that calls refresh_data.sh every weekday at
# 21:00 ET.  Since cron runs in UTC, and ET is UTC-4 (EDT) or UTC-5 (EST),
# we schedule at 01:00 UTC Tuesday–Saturday (which is 21:00 ET Mon–Fri on
# standard time).  During daylight saving this runs at 22:00 ET — still
# well after the 20:00 ET market close, so data is always available.
#
# The job is safe to install multiple times — it replaces itself rather than
# duplicating.
#
# USAGE
#   ./refresh_cron.sh --install     add the cron job (idempotent)
#   ./refresh_cron.sh --remove      remove the cron job
#   ./refresh_cron.sh --status      show whether the job is installed
#   ./refresh_cron.sh --help        print this help
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REFRESH_SCRIPT="${SCRIPT_DIR}/refresh_data.sh"
LOG_FILE="${SCRIPT_DIR}/logs/refresh.log"
CRON_MARKER="nox-data-refresh"

# ---- Validate that the target script actually exists ----------------------
if [ ! -f "$REFRESH_SCRIPT" ]; then
    echo "[CRON] ERROR: refresh_data.sh not found at: $REFRESH_SCRIPT"
    exit 1
fi

# Make sure it's executable
chmod +x "$REFRESH_SCRIPT"

# The actual cron line:
#   01:00 UTC Tue–Sat  →  21:00 ET Mon–Fri (standard time)
CRON_LINE="0 1 * * 2-6 ${REFRESH_SCRIPT} >> ${LOG_FILE} 2>&1   # ${CRON_MARKER}"

# ---------------------------------------------------------------------------
# Argument handling
# ---------------------------------------------------------------------------
ACTION="${1:-}"

case "$ACTION" in
    --install)
        echo "[CRON] Installing Nox data-refresh cron job..."

        # Grab current crontab (ignore error if none exists yet)
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)

        # Remove any previous version of our line
        CLEANED=$(echo "$CURRENT_CRON" | grep -v "$CRON_MARKER" || true)

        # Append the new line
        {
            echo "$CLEANED"
            echo "$CRON_LINE"
        } | crontab -

        echo "[CRON] Installed. Schedule: weekdays at 01:00 UTC (≈ 21:00 ET)."
        echo "[CRON] Cron entry:"
        echo "       $CRON_LINE"
        echo "[CRON] Log file: $LOG_FILE"
        echo ""
        echo "[CRON] To verify:  ./refresh_cron.sh --status"
        echo "[CRON] To remove:  ./refresh_cron.sh --remove"
        ;;

    --remove)
        echo "[CRON] Removing Nox data-refresh cron job..."
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)
        if echo "$CURRENT_CRON" | grep -q "$CRON_MARKER"; then
            echo "$CURRENT_CRON" | grep -v "$CRON_MARKER" | crontab -
            echo "[CRON] Removed."
        else
            echo "[CRON] Job not found — nothing to remove."
        fi
        ;;

    --status)
        CURRENT_CRON=$(crontab -l 2>/dev/null || true)
        if echo "$CURRENT_CRON" | grep -q "$CRON_MARKER"; then
            echo "[CRON] Status: INSTALLED"
            echo "       $(echo "$CURRENT_CRON" | grep "$CRON_MARKER")"
        else
            echo "[CRON] Status: NOT INSTALLED"
            echo "[CRON] Run './refresh_cron.sh --install' to add the job."
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
