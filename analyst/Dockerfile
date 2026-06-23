# ── Minimal Runtime Image ──────────────────────────────────────────────────────
# Copies pre-built binary from the host
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    ca-certificates \
    procps \
    && rm -rf /var/lib/apt/lists/*

# Run as non-root for container security — never execute trading logic as root.
RUN useradd --no-create-home --shell /bin/false appuser

WORKDIR /app

# Copy pre-built binary from analyst directory
COPY analyst/analyst_agent .

RUN chown appuser:appuser analyst_agent && chmod +x analyst_agent

USER appuser

# RULE-006/007 — Detect a hung/dead analyst_agent process.
# The binary is a 24 h sleep-loop; we check that the process is still alive
# every 60 s. Docker Compose / the orchestrator will restart the container
# automatically after 3 consecutive failures (i.e., within ~3 minutes).
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD pgrep -x analyst_agent > /dev/null || exit 1

CMD ["./analyst_agent"]
