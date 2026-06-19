# ── Stage 1: Builder ──────────────────────────────────────────────────────────
# Compiles the analyst binary inside a full build environment.
# The final image receives only the stripped binary — no compiler, no headers.
FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ \
    libssl-dev \
    ca-certificates \
    wget \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# Download vendored single-header libraries — both pinned to explicit release tags.
# httplib: v0.18.5 | nlohmann/json: v3.11.3
RUN mkdir -p nlohmann \
    && wget -q -O httplib.h \
       https://raw.githubusercontent.com/yhirose/cpp-httplib/v0.18.5/httplib.h \
    && wget -q -O nlohmann/json.hpp \
       https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp

COPY analyst/main.cpp ./analyst/
COPY shared/RegimeStateMachine.hpp ./shared/

# Compile with full C++17 + optimisation + required link flags (Build Standards)
RUN cd analyst && g++ -std=c++17 -I.. -O3 main.cpp -o analyst_agent -lssl -lcrypto -lpthread

# ── Stage 2: Runtime ──────────────────────────────────────────────────────────
# Minimal runtime image — only the binary and its shared library dependencies.
FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    libssl3 \
    ca-certificates \
    procps \
    && rm -rf /var/lib/apt/lists/*

# Run as non-root for container security — never execute trading logic as root.
RUN useradd --no-create-home --shell /bin/false appuser

WORKDIR /app
COPY --from=builder /app/analyst_agent .
RUN chown appuser:appuser analyst_agent

USER appuser

# RULE-006/007 — Detect a hung/dead analyst_agent process.
# The binary is a 24 h sleep-loop; we check that the process is still alive
# every 60 s. Docker Compose / the orchestrator will restart the container
# automatically after 3 consecutive failures (i.e., within ~3 minutes).
HEALTHCHECK --interval=60s --timeout=5s --start-period=10s --retries=3 \
    CMD pgrep -x analyst_agent > /dev/null || exit 1

CMD ["./analyst_agent"]
