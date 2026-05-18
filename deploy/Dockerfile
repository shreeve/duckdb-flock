# syntax=docker/dockerfile:1.7
#
# Single-stage container that downloads:
#   1. The DuckDB v1.5.2 CLI matching the container architecture
#   2. The harbor v0.1.0 .duckdb_extension binary for the same arch
#   3. A bootstrap SQL file that LOADs harbor and calls harbor_serve
#
# Build:
#   docker build --tag harbor:0.1.0 -f deploy/Dockerfile .
#
# Run:
#   docker run --rm -p 9494:9494 \
#     -e HARBOR_TOKEN=replace-with-secret \
#     harbor:0.1.0
#
# Or use deploy/docker-compose.yml for a managed setup with persistent
# storage and an explicit bootstrap mount.

FROM debian:12-slim

ARG DUCKDB_VERSION=v1.5.2
ARG HARBOR_VERSION=v0.1.0
# Auto-detect arch by default; override via --build-arg PLATFORM=linux_amd64.
ARG PLATFORM

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates curl unzip jq gosu \
 && rm -rf /var/lib/apt/lists/*

# Resolve platform if not explicitly set: linux_amd64 vs linux_arm64.
RUN set -eux; \
    if [ -z "${PLATFORM:-}" ]; then \
      arch="$(dpkg --print-architecture)"; \
      case "$arch" in \
        amd64) PLATFORM=linux_amd64 ;; \
        arm64) PLATFORM=linux_arm64 ;; \
        *) echo "Unsupported arch: $arch" >&2; exit 1 ;; \
      esac; \
      echo "PLATFORM=$PLATFORM"; \
    fi; \
    echo "$PLATFORM" > /etc/harbor-platform

# Install DuckDB CLI for the target arch.
RUN set -eux; \
    PLATFORM="$(cat /etc/harbor-platform)"; \
    case "$PLATFORM" in \
      linux_amd64) DUCKDB_ARCH="linux-amd64" ;; \
      linux_arm64) DUCKDB_ARCH="linux-arm64" ;; \
    esac; \
    curl -sfL "https://github.com/duckdb/duckdb/releases/download/${DUCKDB_VERSION}/duckdb_cli-${DUCKDB_ARCH}.zip" -o /tmp/duckdb.zip; \
    unzip -j /tmp/duckdb.zip -d /usr/local/bin; \
    rm /tmp/duckdb.zip; \
    chmod +x /usr/local/bin/duckdb; \
    duckdb -version

# Install the harbor extension binary for this arch.
RUN set -eux; \
    PLATFORM="$(cat /etc/harbor-platform)"; \
    mkdir -p /var/lib/harbor; \
    curl -sfL "https://github.com/shreeve/duckdb-harbor/releases/download/${HARBOR_VERSION}/harbor.${PLATFORM}.duckdb_extension" \
         -o /var/lib/harbor/harbor.duckdb_extension; \
    chmod +x /var/lib/harbor/harbor.duckdb_extension

# Non-root user.
RUN useradd --system --home-dir /var/lib/harbor --shell /usr/sbin/nologin harbor; \
    chown -R harbor:harbor /var/lib/harbor

# A minimal bootstrap that uses the HARBOR_TOKEN env var as a seed
# token. For production, mount a richer bootstrap on top of this file
# (see docker-compose.yml).
COPY deploy/harbor-bootstrap-docker.sql /var/lib/harbor/harbor-bootstrap.sql

EXPOSE 9494
USER harbor
WORKDIR /var/lib/harbor

# `-no-stdin` matters so the CLI doesn't think it should read from a
# (closed) terminal and exit. The bootstrap script ends with
# `CALL harbor_wait();` so the duckdb process blocks until SIGTERM.
CMD ["sh", "-c", "duckdb -unsigned -no-stdin < /var/lib/harbor/harbor-bootstrap.sql"]
