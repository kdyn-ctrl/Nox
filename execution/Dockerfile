# ── Minimal Runtime Image ──────────────────────────────────────────────────────
# Copies pre-built binary from the host
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    libsqlite3-0 \
    ca-certificates \
    procps \
    && rm -rf /var/lib/apt/lists/*

# Run as non-root for container security — never execute trading logic as root.
RUN useradd --no-create-home --shell /bin/false appuser

WORKDIR /app

# Copy pre-built binary from execution directory
COPY execution/nox_engine .

RUN chown -R appuser:appuser /app && chmod +x nox_engine

USER appuser

# Detect a hung/dead engine process.
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD pgrep -x nox_engine > /dev/null || exit 1

EXPOSE 8080
CMD ["./nox_engine"]
