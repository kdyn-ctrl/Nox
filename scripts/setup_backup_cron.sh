#!/bin/bash
# Setup cron job for the nightly WAL-safe DB backup (audit §5 C2 / RULE-D10).
# Idempotent: re-running will not add a duplicate entry.
#
# Default schedule is nightly 03:00 (local host time). To back up more often,
# edit CRON_EXPRESSION below (e.g. "0 * * * *" for hourly) — the backup script
# itself is interval-agnostic and prunes by NOX_BACKUP_RETENTION_DAYS.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKUP_SCRIPT="${SCRIPT_DIR}/backup_db.sh"
LOG_FILE="/var/log/nox_db_backup.log"

if [ ! -x "$BACKUP_SCRIPT" ]; then
    echo "❌ Error: ${BACKUP_SCRIPT} not found or not executable."
    echo "   Run: chmod +x ${BACKUP_SCRIPT}"
    exit 1
fi

sudo mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
sudo touch "$LOG_FILE" 2>/dev/null || LOG_FILE="/tmp/nox_db_backup.log"

CRON_EXPRESSION="0 3 * * *"

if crontab -l 2>/dev/null | grep -q "backup_db.sh"; then
    echo "⚠️  A backup_db.sh cron entry already exists. Skipping install."
    crontab -l | grep "backup_db.sh"
    exit 0
fi

(
    crontab -l 2>/dev/null || echo ""
    echo "${CRON_EXPRESSION} ${BACKUP_SCRIPT} >> ${LOG_FILE} 2>&1"
) | crontab -

echo "✅ DB backup cron installed."
echo "   Schedule: nightly 03:00 (host local time)"
echo "   Log:      ${LOG_FILE}"
echo "   Backups:  \${NOX_BACKUP_DIR:-<repo>/data/backups}"
echo ""
echo "To remove: crontab -e  (delete the line containing 'backup_db.sh')"
