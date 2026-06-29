#!/bin/bash
# Setup cron job to run daily report at 4 PM ET (market close)

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
LOG_FILE="/var/log/nox_daily_report.log"

# Check if script exists
if [ ! -f "$SCRIPT_DIR/daily_report.py" ]; then
    echo "❌ Error: daily_report.py not found at $SCRIPT_DIR"
    exit 1
fi

# Create log directory if needed
sudo mkdir -p "$(dirname "$LOG_FILE")" 2>/dev/null || true
sudo touch "$LOG_FILE" 2>/dev/null || LOG_FILE="/tmp/nox_daily_report.log"

# Create wrapper script
WRAPPER_SCRIPT="$SCRIPT_DIR/run_daily_report.sh"
cat > "$WRAPPER_SCRIPT" << 'EOF'
#!/bin/bash
# Wrapper to run daily report with proper environment
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
python3 scripts/daily_report.py --engine-url http://localhost:8080
EOF

chmod +x "$WRAPPER_SCRIPT"

# Setup cron job
# 4:00 PM ET = 16:00 in 24h format
# Run Monday-Friday only (1-5)
CRON_EXPRESSION="0 16 * * 1-5"

# Check if cron entry already exists
if crontab -l 2>/dev/null | grep -q "run_daily_report.sh"; then
    echo "⚠️  Cron entry already exists. Skipping..."
    crontab -l
    exit 0
fi

# Add cron job
(
    crontab -l 2>/dev/null || echo ""
    echo "$CRON_EXPRESSION $WRAPPER_SCRIPT >> $LOG_FILE 2>&1"
) | crontab -

echo "✅ Cron job installed successfully!"
echo ""
echo "Cron Schedule: Daily at 4:00 PM ET (weekdays only)"
echo "Log file: $LOG_FILE"
echo ""
echo "Current crontab:"
crontab -l | grep "run_daily_report"
echo ""
echo "To remove this cron job, run:"
echo "  crontab -e"
echo "  # and remove the line containing 'run_daily_report.sh'"
