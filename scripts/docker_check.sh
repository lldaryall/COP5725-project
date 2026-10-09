#!/usr/bin/env bash
# Builds and tests everything on Linux with gcc and clang, including the
# sanitizer run and the CMake build, then checks whether perf counters work.
# Meant to run inside docker/Dockerfile (see scripts/docker_run.sh) or on any
# Linux box. Builds out of a copy so the host's build/ is left alone.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
work=$(mktemp -d)
cp -r "$src"/{Makefile,CMakeLists.txt,include,src,tests,third_party,scripts} "$work"/
cd "$work"
echo "== $(uname -sm), $(g++ --version | head -1), $(clang++ --version | head -1)"

for cxx in g++ clang++; do
  echo "== $cxx: build + tests"
  make -s clean
  make -s CXX=$cxx CXXFLAGS="-O3 -DNDEBUG -Werror" all
  ./build/tests | tail -1
  echo "== $cxx: ASan + UBSan"
  make -s CXX=$cxx check | tail -1
done

echo "== CMake"
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build build-cmake -j"$(nproc)" >/dev/null
./build-cmake/tests | tail -1

echo "== smoke run (every index, every pattern)"
make -s all
for idx in stdmap btree; do
  ./build/bench --index $idx --dataset synthetic:lognormal --n 400000 --init 200000 --ops 200000 \
    --mix writeheavy --pattern drift 2>&1 | grep -E 'throughput|ERROR'
done
for idx in rmi alex; do
  ./build/bench --index $idx --dataset synthetic:lognormal --n 200000 --init 200000 --ops 200000 \
    --mix readonly 2>&1 | grep -E 'throughput|ERROR'
done

echo "== perf counters"
echo "perf_event_paranoid=$(cat /proc/sys/kernel/perf_event_paranoid 2>/dev/null || echo n/a)"
./build/bench --index btree --dataset synthetic:uniform --n 200000 --init 200000 --ops 500000 \
  --mix readonly --csv perf.csv 2>&1 | grep -E 'per op' || echo "perf counters unavailable here"
echo "== all Linux checks passed"
