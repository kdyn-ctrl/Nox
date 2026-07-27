#!/bin/bash
# =============================================================================
# backup_db.sh — WAL-safe hot backup of the single SQLite state DB
# =============================================================================
# Audit §5 C2 / RULE-D10: memory_bank.db is the ENTIRE system state (position
# truth, trade history, calibration), written by 3+ processes, with zero backup
# mechanism — one corruption loses everything unrecoverably.
#
# Uses SQLite's online `.backup` (not `cp`): the backup API is safe against
# concurrent writers in WAL mode and cannot capture a torn page mid-write. Each
# copy is integrity-checked before it is trusted; a copy that fails the check is
# deleted and the script exits non-zero so the cron log / dead-man's-switch sees
# the failure (a silently-broken backup is worse than none).
#
# Tunables (RULE-D11; fake-safe defaults — the branch is a harmless no-op if a
# var is unset):
#   NOX_DB_PATH               default: <repo>/data/memory_bank.db
#   NOX_BACKUP_DIR            default: <repo>/data/backups
#   NOX_BACKUP_RETENTION_DAYS default: 14   (copies older than this are pruned)
#
# USAGE
#   ./backup_db.sh              # take one backup + prune old ones
#   ./backup_db.sh --help
# =============================================================================

set -euo pipefail

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    sed -n '/^# ====/,/^# ====/p' "$0" | grep '^#' | sed 's/^# \?//'
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"

DB_PATH="${NOX_DB_PATH:-${REPO_ROOT}/data/memory_bank.db}"
BACKUP_DIR="${NOX_BACKUP_DIR:-${REPO_ROOT}/data/backups}"
RETENTION_DAYS="${NOX_BACKUP_RETENTION_DAYS:-14}"

log() { echo "[BACKUP] $(date -u '+%Y-%m-%d %H:%M:%S UTC') $*"; }

if ! command -v sqlite3 >/dev/null 2>&1; then
    log "ERROR: sqlite3 not found on PATH — cannot back up. Install sqlite3."
    exit 1
fi

if [ ! -f "$DB_PATH" ]; then
    log "ERROR: DB not found at ${DB_PATH} (wrong CWD? bad NOX_DB_PATH?)."
    exit 1
fi

# Guard against the stale 0-byte repo-root DB footgun (audit §0): never back up
# an empty file and pass it off as state.
if [ ! -s "$DB_PATH" ]; then
    log "ERROR: DB at ${DB_PATH} is 0 bytes — refusing to back up empty state."
    exit 1
fi

mkdir -p "$BACKUP_DIR"
TIMESTAMP="$(date -u '+%Y%m%d_%H%M%S')"
DEST="${BACKUP_DIR}/memory_bank_${TIMESTAMP}.db"

log "Backing up ${DB_PATH} -> ${DEST}"
if ! sqlite3 "$DB_PATH" ".backup '${DEST}'"; then
    log "ERROR: .backup command failed."
    rm -f "$DEST"
    exit 1
fi

# A backup that won't pass integrity_check is not a backup.
CHECK="$(sqlite3 "$DEST" 'PRAGMA integrity_check;' 2>&1 || true)"
if [ "$CHECK" != "ok" ]; then
    log "ERROR: integrity_check on backup failed: ${CHECK}. Deleting bad copy."
    rm -f "$DEST"
    exit 1
fi

SIZE="$(du -h "$DEST" | cut -f1)"
log "OK: verified backup (${SIZE})."

# Prune copies older than the retention window (never touches anything else).
PRUNED="$(find "$BACKUP_DIR" -maxdepth 1 -name 'memory_bank_*.db' -mtime "+${RETENTION_DAYS}" -print -delete | wc -l | tr -d ' ')"
log "Pruned ${PRUNED} backup(s) older than ${RETENTION_DAYS} day(s)."

COUNT="$(find "$BACKUP_DIR" -maxdepth 1 -name 'memory_bank_*.db' | wc -l | tr -d ' ')"
log "Done. ${COUNT} backup(s) retained in ${BACKUP_DIR}."
