#!/bin/bash
# =============================================================================
# weekend_batch.sh — Nox Weekend Skeptic Report Batch (WS6)
# =============================================================================
# Orchestrates the Saturday-morning Skeptic Report run.  Fires inside the
# america-data-engine Docker container (which already has all WS1-3 modules
# and their dependencies) and writes JSON + Markdown to data/.
#
# The companion script  refresh_cron.sh --install  installs a Saturday
# 08:00 UTC (~04:00 ET) crontab entry that calls this script automatically.
#
# USAGE
#   ./weekend_batch.sh              # run now (containers must be up)
#   ./weekend_batch.sh --dry-run    # validate container reachability only
#   ./weekend_batch.sh --help       # print this help
#
# OUTPUT
#   data/skeptic_report_YYYY-MM-DD.json
#   data/skeptic_report_YYYY-MM-DD.md
#   logs/weekend_batch.log          (appended each run)
#
# REQUIREMENTS
#   - docker compose up -d must be running (america-data-engine container).
#   - data/ directory must exist at repo root (created by heartbeat-monitor).
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
LOG_DIR="${REPO_ROOT}/logs"
LOG_FILE="${LOG_DIR}/weekend_batch.log"
CONTAINER="nox_america_data_engine"
REPORT_SCRIPT="/app/generate_skeptic_report.py"

mkdir -p "$LOG_DIR"

# All subsequent output goes to both stdout and the log file
exec > >(tee -a "$LOG_FILE") 2>&1

TIMESTAMP="$(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo ""
echo "========================================================"
echo "  Nox Weekend Skeptic Batch  —  ${TIMESTAMP}"
echo "========================================================"

# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------
DRY_RUN=0
for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --help|-h)
            sed -n '/^# ====/,/^# ====/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "[BATCH] Unknown argument: $arg"
            exit 1
            ;;
    esac
done

# ---------------------------------------------------------------------------
# Verify container is running
# ---------------------------------------------------------------------------
echo "[BATCH] Checking container: ${CONTAINER}..."
if ! docker inspect --format '{{.State.Running}}' "${CONTAINER}" 2>/dev/null | grep -q "true"; then
    echo "[BATCH] ERROR: Container '${CONTAINER}' is not running."
    echo "[BATCH]        Start with: docker compose up -d"
    exit 1
fi
echo "[BATCH] Container is up."

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[BATCH] Dry-run mode — container reachable. Exiting without generating report."
    exit 0
fi

# ---------------------------------------------------------------------------
# Run the report generator inside the container
# ---------------------------------------------------------------------------
echo "[BATCH] Launching Skeptic Report inside ${CONTAINER}..."
echo "[BATCH] Script: ${REPORT_SCRIPT}"

set +e
docker exec -t "${CONTAINER}" python "${REPORT_SCRIPT}"
EXIT_CODE=$?
set -e

echo ""
case "$EXIT_CODE" in
    0)
        echo "[BATCH] SUCCESS — report written to data/."
        ;;
    1)
        echo "[BATCH] PARTIAL — report written but one or more pipeline stages failed."
        echo "[BATCH] Review pipeline_errors in the JSON report and logs above."
        ;;
    *)
        echo "[BATCH] ERROR — report generator exited with code ${EXIT_CODE}."
        ;;
esac

echo "[BATCH] Finished at $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================================"
exit "$EXIT_CODE"
