FROM python:3.11-slim
WORKDIR /app

# Pin every dependency so builds are fully reproducible (RULE-009 / scan #17).
# schedule 1.2.2  — minimum 1.2.0 required: that release introduced the
#                   tz-argument form .at("HH:MM", tz) used in schedule_checker.
#                   Without this pin an older version could be resolved and the
#                   whole scheduler thread would crash silently at runtime.
RUN pip install --no-cache-dir \
    requests==2.32.3 \
    pyTelegramBotAPI==4.21.0 \
    anthropic==0.40.0 \
    schedule==1.2.2 \
    beautifulsoup4==4.12.3 \
    lxml==5.3.0

# Create the data directory that DB_PATH ('/app/data/memory_bank.db') depends on.
# Without this sqlite3.connect() raises OperationalError at runtime.
# Declared as a VOLUME so the DB survives container restarts when mounted.
RUN mkdir -p /app/data
VOLUME ["/app/data"]

COPY monitor.py .

CMD ["python", "-u", "monitor.py"]
