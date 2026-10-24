#!/bin/bash
# usage: tools/run_plain.sh P M WAIT [tc]
# Like run_stress.sh but without signal interference: start P processes, wait WAIT seconds, SIGTERM
# all, validate. Runs bin/da_proc directly (like stress.py), since run.sh would swallow the signal. Hosts/config match stress.py (ids 1..P on ports 11001.., receiver = 1).
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
P=$1; M=$2; WAIT=$3; TC=$4
LOGS="$ROOT/runs/$(date +%Y%m%d-%H%M%S)-plain-p$P-m$M${TC:+-tc}"
mkdir -p "$LOGS"
for i in $(seq 1 "$P"); do echo "$i localhost $((11000 + i))"; done > "$LOGS/hosts"
echo "$M 1" > "$LOGS/config"
if [ "$TC" = tc ]; then
  sudo tc qdisc add dev lo root netem delay 200ms 50ms loss 10% 25% reorder 25% 50%
  trap 'sudo tc qdisc del dev lo root' EXIT
fi
pids=()
for i in $(seq 1 "$P"); do
  "$ROOT/template_cpp/bin/da_proc" --id "$i" --hosts "$LOGS/hosts" --output "$LOGS/proc$(printf %02d "$i").output" "$LOGS/config" \
    >"$LOGS/proc$(printf %02d "$i").stdout" 2>"$LOGS/proc$(printf %02d "$i").stderr" &
  pids+=($!)
done
sleep "$WAIT"
kill -TERM "${pids[@]}" 2>/dev/null
wait
python3 "$ROOT/tools/validate_pl.py" "$LOGS" --crashed
