#!/bin/bash
# Build, clean and zip template_cpp/ contents into submission.zip at the repo root.
# Usage: tools/package.sh [TEST]   TEST (0-6) also writes test.txt for the Moodle testing milestone;
#                                  omit it for the official submission (no test.txt).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TPL="$ROOT/template_cpp"
ZIP="$ROOT/submission.zip"

if [ $# -gt 0 ] && ! [[ "$1" =~ ^[0-6]$ ]]; then
  echo "TEST must be a single integer 0-6" >&2
  exit 1
fi

if [ -n "$(git -C "$ROOT" status --porcelain)" ]; then
  echo "WARNING: working tree not clean; the zip will not match a commit" >&2
fi
echo "commit: $(git -C "$ROOT" rev-parse --short HEAD)"

log="$(mktemp)"
if ! "$TPL/build.sh" >"$log" 2>&1; then
  cat "$log" >&2
  echo "build failed" >&2
  exit 1
fi
"$TPL/cleanup.sh"

rm -f "$ZIP" "$ROOT/test.txt"
(cd "$TPL" && python3 -m zipfile -c "$ZIP" bin build.sh cleanup.sh CMakeLists.txt run.sh src)

# Structure check: scripts at top level and executable, no build artifacts.
python3 - "$ZIP" <<'EOF'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
names = set(z.namelist())
for f in ["build.sh", "run.sh", "cleanup.sh", "CMakeLists.txt", "src/CMakeLists.txt"]:
    assert f in names, f"missing {f}"
    if f.endswith(".sh"):
        assert (z.getinfo(f).external_attr >> 16) & 0o111, f"{f} not executable"
bad = [n for n in names if n.startswith("target/") or n == "bin/da_proc"]
assert not bad, f"build artifacts in zip: {bad}"
print(f"zip structure OK ({len(names)} entries)")
EOF

# Build from the extracted zip and check 3 processes stay up for 1 s.
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp" "$log"' EXIT
python3 -m zipfile -e "$ZIP" "$tmp"
chmod +x "$tmp"/*.sh  # Python's extractor drops file permissions
if ! "$tmp/build.sh" >"$log" 2>&1; then
  cat "$log" >&2
  echo "build from zip failed" >&2
  exit 1
fi
pids=()
for i in 1 2 3; do
  "$tmp/bin/da_proc" --id "$i" --hosts "$ROOT/example/hosts" --output "$tmp/$i.output" \
    "$ROOT/example/configs/perfect-links.config" >/dev/null 2>&1 &
  pids+=($!)
done
sleep 1
for p in "${pids[@]}"; do
  kill -0 "$p" 2>/dev/null || { echo "process $p died early" >&2; exit 1; }
done
kill -TERM "${pids[@]}"
wait "${pids[@]}" 2>/dev/null || true
echo "build + 3-process smoke run from zip OK"

if [ $# -gt 0 ]; then
  printf '%d\n' "$1" >"$ROOT/test.txt"
  echo "test.txt = $1"
fi
echo "ready: $ZIP"
