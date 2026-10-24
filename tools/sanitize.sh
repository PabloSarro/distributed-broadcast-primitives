#!/bin/bash
# usage: tools/sanitize.sh [P M WAIT]   (default 4 3000 60)
# Builds template_cpp/src with ASan+UBSan into runs/san/ (the real build is untouched), runs one
# stress test through the validator, and fails if any sanitizer report shows up in the stderr files.
# ThreadSanitizer is not used: it aborts at startup on this WSL kernel (ASLR mapping).
set -e
# The template Parser leaks the getaddrinfo result once at startup; not ours, and harmless.
export ASAN_OPTIONS=detect_leaks=0
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SAN="$ROOT/runs/san"
mkdir -p "$SAN/bin"
cp "$ROOT/template_cpp/run.sh" "$SAN/run.sh"
g++ -std=c++17 -O1 -g -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer \
  -Wall -Wextra -I"$ROOT/template_cpp/src/include" "$ROOT/template_cpp/src/src/main.cpp" -pthread -o "$SAN/bin/da_proc"
RUNSCRIPT="$SAN/run.sh" "$ROOT/tools/run_stress.sh" "${1:-4}" "${2:-3000}" "${3:-60}" | tee /tmp/san.out | tail -3
LOGS=$(ls -dt "$ROOT"/runs/2* | head -1)
if grep -lE "Sanitizer|runtime error" "$LOGS"/*.stderr; then echo "SANITIZER REPORT in $LOGS"; exit 1; fi
echo "sanitizer clean: $LOGS"
