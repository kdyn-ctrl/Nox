FROM python:3.11-slim
WORKDIR /app

# build-essential is required to compile C extensions pulled in by some
# akshare / pandas dependencies on the slim base image. The layer is
# discarded after pip install to keep the final image lean.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    && rm -rf /var/lib/apt/lists/*

# Copy and install dependencies before copying source so that Docker's layer
# cache is invalidated by requirements changes but NOT by source-only edits.
COPY requirements.txt .
RUN pip install --no-cache-dir -r requirements.txt

COPY scrapers.py main.py retry_utils.py edgar_cn_lag.py ./

# Port 8000 is the internal-only china-data-engine port. It is NOT exposed to the
# host — only reachable by other containers on the nox_net bridge network.
EXPOSE 8000

# -u disables Python output buffering so all print() logs appear immediately
# in `docker compose logs` without waiting for a flush.
CMD ["uvicorn", "main:app", "--host", "0.0.0.0", "--port", "8000"]
