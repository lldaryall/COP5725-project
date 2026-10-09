#!/usr/bin/env bash
# Runs scripts/docker_check.sh in the Linux container. --privileged lets the
# container use perf_event_open (hardware counters still depend on the VM).
set -euo pipefail
cd "$(dirname "$0")/.."
docker build -q -t cop5725 docker >/dev/null
docker run --rm --privileged -v "$PWD":/src:ro cop5725 bash scripts/docker_check.sh
