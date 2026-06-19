#!/bin/bash
# =============================================================================
# refresh_data.sh — Nox Automated Data Refresh
# =============================================================================
# Keeps data/spy_vix_daily_v2.csv current with the latest market close.
#
# DESIGN
#   - Idempotent: safe to call any number of times.  If the file is already
#     current the script exits 0 immediately without hitting Yahoo Finance.
#   - Exit codes mirror download_data.py:
#       0  — data refreshed (or already current)
#       1  — download/validation error  (DOES NOT overwrite existing file)
#       3  — dependency missing (Python / yfinance / pandas not found)
#
# USAGE
#   ./refresh_data.sh              # smart: skip if already fresh
#   ./refresh_data.sh --force      # always re-download
#   ./refresh_data.sh --no-backup  # skip .bak file creation
#   ./refresh_data.sh --help       # show this help
#
# AUTOMATED (cron / systemd-timer)
#   The companion script  refresh_cron.sh  installs a crontab entry that
#   calls this script every weekday at 21:00 ET (01:00 UTC the next day).
#   Run:  ./refresh_cron.sh --install
#
# LOG FILE
#   All output is tee'd to  logs/refresh.log  (created automatically).
# =============================================================================

set -euo pipefail

# ---------------------------------------------------------------------------
# Paths — resolve relative to this script's location so cron works from any
# working directory.
# ---------------------------------------------------------------------------
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOWNLOADER="${SCRIPT_DIR}/download_data.py"
DATA_FILE="${SCRIPT_DIR}/data/spy_vix_daily_v2.csv"
LOG_DIR="${SCRIPT_DIR}/logs"
LOG_FILE="${LOG_DIR}/refresh.log"

PYTHON_CMD=""

# ---------------------------------------------------------------------------
# Argument parsing — pass anything we don't understand straight to the Python
# script so new flags work without touching this file.
# ---------------------------------------------------------------------------
SHOW_HELP=0
PASSTHROUGH_ARGS=()
for arg in "$@"; do
    case "$arg" in
        --help|-h)
            SHOW_HELP=1
            ;;
        *)
            PASSTHROUGH_ARGS+=("$arg")
            ;;
    esac
done

if [ "$SHOW_HELP" -eq 1 ]; then
    sed -n '/^# ====/,/^# ====/p' "$0" | grep '^#' | sed 's/^# \?//'
    exit 0
fi

# ---------------------------------------------------------------------------
# Ensure log directory exists
# ---------------------------------------------------------------------------
mkdir -p "$LOG_DIR"

# ---------------------------------------------------------------------------
# All subsequent output goes to both stdout and the log file
# ---------------------------------------------------------------------------
exec > >(tee -a "$LOG_FILE") 2>&1

TIMESTAMP="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
echo "========================================================"
echo "  Nox Data Refresh  —  ${TIMESTAMP}"
echo "========================================================"

# ---------------------------------------------------------------------------
# Locate a Python 3 interpreter that has yfinance and pandas
# ---------------------------------------------------------------------------
for cmd in python3 python; do
    if command -v "$cmd" &>/dev/null; then
        version=$("$cmd" --version 2>&1 | grep -oP '\d+\.\d+' | head -1)
        major="${version%%.*}"
        if [ "$major" -ge 3 ] 2>/dev/null; then
            # Quick import check — avoids a confusing traceback for the user
            if "$cmd" -c "import yfinance, pandas" 2>/dev/null; then
                PYTHON_CMD="$cmd"
                break
            else
                echo "[REFRESH] [WARN] ${cmd} found but yfinance/pandas missing — trying next..."
            fi
        fi
    fi
done

if [ -z "$PYTHON_CMD" ]; then
    echo "[REFRESH] [FATAL] No Python 3 with yfinance + pandas found."
    echo "[REFRESH]         Install with:  pip install yfinance pandas"
    exit 3
fi

echo "[REFRESH] Using Python: $($PYTHON_CMD --version 2>&1)"
echo "[REFRESH] Data file:    ${DATA_FILE}"

# ---------------------------------------------------------------------------
# Run the downloader
# ---------------------------------------------------------------------------
set +e
"$PYTHON_CMD" "$DOWNLOADER" "${PASSTHROUGH_ARGS[@]+"${PASSTHROUGH_ARGS[@]}"}"
EXIT_CODE=$?
set -e

# ---------------------------------------------------------------------------
# Interpret exit codes
# ---------------------------------------------------------------------------
case "$EXIT_CODE" in
    0)
        echo "[REFRESH] SUCCESS — data file is current."
        ;;
    2)
        # Exit code 2 = "already fresh, no action taken" — treat as success
        echo "[REFRESH] SKIPPED — file already up-to-date."
        EXIT_CODE=0
        ;;
    1)
        echo "[REFRESH] ERROR — download or validation failed.  Existing file preserved."
        echo "[REFRESH] Check logs above or re-run with --force after investigating."
        ;;
    3)
        echo "[REFRESH] DEPENDENCY ERROR — see message above."
        ;;
    *)
        echo "[REFRESH] UNEXPECTED exit code ${EXIT_CODE} from downloader."
        ;;
esac

echo "[REFRESH] Finished at $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================================"
exit "$EXIT_CODE"
