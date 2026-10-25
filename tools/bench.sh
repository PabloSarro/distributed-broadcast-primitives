#!/bin/bash
# usage: tools/bench.sh P SECONDS
# Throughput benchmark: P processes pinned to 2 cores (like the grader), config "2147483647 1" (receiver = 1,
# senders never run out of messages), no tc. After SECONDS: record RSS, SIGTERM all, report receiver d-lines/s.
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P=$1; SECS=$2
LOGS="$ROOT/runs/$(date +%Y%m%d-%H%M%S)-bench-p$P"
mkdir -p "$LOGS"
for i in $(seq 1 "$P"); do echo "$i localhost $((11000 + i))"; done > "$LOGS/hosts"
echo "2147483647 1" > "$LOGS/config"
pids=()
for i in $(seq 1 "$P"); do
  taskset -c "${CPUS:-0,1}" "${BIN:-$ROOT/template_cpp/bin/da_proc}" --id "$i" --hosts "$LOGS/hosts" \
    --output "$LOGS/proc$(printf %02d "$i").output" "$LOGS/config" >/dev/null 2>"$LOGS/proc$(printf %02d "$i").stderr" &
  pids+=($!)
done
sleep "$SECS"
maxrss=0; ticks=0
for p in "${pids[@]}"; do
  kb=$(awk '/VmHWM/ {print $2}' "/proc/$p/status" 2>/dev/null)
  [ "${kb:-0}" -gt "$maxrss" ] && maxrss=$kb
  t=$(awk '{print $14 + $15}' "/proc/$p/stat" 2>/dev/null)
  ticks=$((ticks + ${t:-0}))
done
kill -TERM "${pids[@]}" 2>/dev/null
wait
d=$(grep -c '^d ' "$LOGS/proc01.output")
mib=$(du -b "$LOGS/proc01.output" | awk '{printf "%.1f", $1/1048576}')
cpu_s=$((ticks / $(getconf CLK_TCK)))
echo "P=$P secs=$SECS d_lines=$d msgs_per_s=$((d / SECS)) cpu_s=$cpu_s msgs_per_cpu_s=$((d / (cpu_s + 1))) out_MiB=$mib max_rss_MiB=$((maxrss / 1024))"
echo "logs: $LOGS"
