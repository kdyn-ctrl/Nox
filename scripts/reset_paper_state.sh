#!/bin/bash
# =============================================================================
# reset_paper_state.sh — Fresh paper-trading account restart
# =============================================================================
# Backs up (never deletes) the local trade ledger and position-tracking state
# so a new Alpaca paper account starts clean. Schema is recreated automatically
# on container startup via the existing idempotent CREATE TABLE IF NOT EXISTS
# init logic — no code changes needed, this is purely a state-move + restart.
#
# Before running this: generate a NEW Alpaca paper API key/secret in the
# Alpaca dashboard and update ALPACA_API_KEY / ALPACA_SECRET_KEY in .env.
# This script does not (and cannot) rotate credentials for you.
#
# USAGE
#   ./reset_paper_state.sh              # stop, back up, restart
#   ./reset_paper_state.sh --dry-run    # show what would be moved, no changes
#   ./reset_paper_state.sh --help       # print this help
#
# OUTPUT
#   data/backup_<UTC timestamp>/memory_bank.db[-wal|-shm]
#   data/backup_<UTC timestamp>/equity_positions.json
#   data/backup_<UTC timestamp>/china_positions.json
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
DATA_DIR="${REPO_ROOT}/data"

for arg in "$@"; do
    case "$arg" in
        --dry-run) DRY_RUN=1 ;;
        --help|-h)
            sed -n '/^# ====/,/^# ====/p' "$0" | grep '^#' | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "[RESET] Unknown argument: $arg"
            exit 1
            ;;
    esac
done
DRY_RUN="${DRY_RUN:-0}"

TIMESTAMP="$(date -u '+%Y%m%d_%H%M%S')"
BACKUP_DIR="${DATA_DIR}/backup_${TIMESTAMP}"

echo "========================================================"
echo "  Nox Paper Account Reset — $(date -u '+%Y-%m-%d %H:%M:%S UTC')"
echo "========================================================"

FILES_TO_MOVE=(
    "memory_bank.db"
    "memory_bank.db-wal"
    "memory_bank.db-shm"
    "equity_positions.json"
    "china_positions.json"
)

echo "[RESET] Files that will be moved to ${BACKUP_DIR}:"
for f in "${FILES_TO_MOVE[@]}"; do
    [ -f "${DATA_DIR}/${f}" ] && echo "  - ${f}"
done

if [ "$DRY_RUN" -eq 1 ]; then
    echo "[RESET] Dry-run mode — no containers stopped, no files moved."
    exit 0
fi

read -rp "[RESET] Have you already generated a NEW Alpaca paper API key and updated .env? [y/N] " CONFIRM
if [[ ! "$CONFIRM" =~ ^[Yy]$ ]]; then
    echo "[RESET] Aborting — update .env with the new ALPACA_API_KEY/ALPACA_SECRET_KEY first."
    exit 1
fi

echo "[RESET] Stopping execution-engine and heartbeat-monitor..."
(cd "$REPO_ROOT" && docker compose stop execution-engine heartbeat-monitor)

mkdir -p "$BACKUP_DIR"
for f in "${FILES_TO_MOVE[@]}"; do
    if [ -f "${DATA_DIR}/${f}" ]; then
        mv "${DATA_DIR}/${f}" "${BACKUP_DIR}/${f}"
        echo "[RESET] Moved ${f} -> ${BACKUP_DIR}/"
    fi
done

echo "[RESET] Restarting execution-engine and heartbeat-monitor..."
(cd "$REPO_ROOT" && docker compose up -d execution-engine heartbeat-monitor)

echo "[RESET] Done. Prior state preserved at: ${BACKUP_DIR}"
echo "[RESET] Verify with: docker logs -f nox_execution"
echo "========================================================"
