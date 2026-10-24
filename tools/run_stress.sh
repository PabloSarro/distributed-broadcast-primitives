#!/bin/bash
# usage: tools/run_stress.sh P M WAIT [tc]
# Runs stress.py perfect with P processes and M messages, waits WAIT seconds, then validates.
# RUNSCRIPT=path overrides the run.sh used (see sanitize.sh). With `tc`, enables the netem rules on lo for the run (asks for sudo).
set -e
P=$1; M=$2; WAIT=$3; TC=$4
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
LOGS="$ROOT/runs/$(date +%Y%m%d-%H%M%S)-p$P-m$M${TC:+-tc}"
export LOGS
mkdir -p "$LOGS"
if [ "$TC" = tc ]; then
  sudo tc qdisc add dev lo root netem delay 200ms 50ms loss 10% 25% reorder 25% 50%
  trap 'sudo tc qdisc del dev lo root' EXIT
fi
(sleep "$WAIT"; echo) | python3 "$ROOT/tools/stress.py" perfect -r "${RUNSCRIPT:-$ROOT/template_cpp/run.sh}" -l "$LOGS" -p "$P" -m "$M" | tee "$LOGS/stress.log"
python3 "$ROOT/tools/validate_pl.py" "$LOGS"
