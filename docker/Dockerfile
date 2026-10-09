# Linux build/benchmark environment (Ubuntu 24.04, gcc 13, clang 18).
#   docker build -t cop5725 docker
#   docker run --rm -v "$PWD":/src cop5725 scripts/docker_check.sh
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
      build-essential clang libclang-rt-18-dev cmake make git curl ca-certificates zstd python3 \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
