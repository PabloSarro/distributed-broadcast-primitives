# Milestone 1 — Perfect Links (deadline: Oct 30 2026, 23:59)

Spec: `../info/project/proj_description.md` §1.2, §2.3, §2.4, FAQ. Grading: correctness 3.5/6 (must pass first), performance 2.5/6 (aggregate delivered msgs / fixed time, ranked vs other C/C++ submissions).

## Hard constraints (checked before every submission)
- [ ] C++17, CMake template unchanged outside `template_cpp/src/`; builds warning-free with the template's `-Werror` flags (gcc 11.4, cmake 3.22).
- [ ] No 3rd-party libs. UDP only, point-to-point, **one UDP socket per process** (bound to own port, used for send + receive).
- [ ] ≤ 8 messages per packet. Payload treated as opaque bytes; never use that payloads are sequential or that `m` is known.
- [ ] ≤ 8 threads per process; 2 cores / 4 GiB shared by up to 128 processes → **no busy-waiting**, small per-process memory.
- [ ] SIGTERM/SIGINT: stop all network activity immediately, then only write the output file. SIGSTOP/SIGCONT may pause us at any time.
- [ ] Output: `b seq` (sender, logged **before** the message first hits the wire), `d sender seq` (receiver, once per message). Whole lines only, `\n`-terminated, no duplicates, file ≤ 64 MiB. Never create/use `deploy/` or `logs/`.
- [ ] Little stdout/stderr (trimmed at 1 MiB): no per-message prints.
- [ ] Credit any code taken from the internet with a link comment (§3.1).

## Design (target, built incrementally)
- **Threads**: main (blocks SIGTERM/SIGINT in all threads, waits with `sigwait`, runs shutdown), receiver thread, sender/retransmit thread. Logger flushing piggybacks on these (separate writer thread only if profiling says so).
- **Wire format** (fixed byte order):
  - DATA: `type=1 | u8 sender_id | u8 k (≤8) | k × (u32 seq | u16 len | payload)`
  - ACK: `type=2 | u8 sender_id | u32 cumulative (all seq < it delivered) | u8 k | k × u32 seq` (acks the packet just received)
- **Sender** (per destination; M1 has one): sliding window of `W` slots (ring indexed by `seq % W`), slot = {in-flight + last-send time | acked}. Window base advances on contiguous acks / cumulative ack. Payload regenerated from seq on retransmit (FAQ Q5) → nothing stored per message. New messages are batched 8 per packet.
- **Retransmission**: adaptive RTO from ack RTTs (EWMA srtt/rttvar, Karn's rule, RTO = srtt + 4·rttvar, clamped, exponential backoff on timeout). Reference: RFC 6298 (https://www.rfc-editor.org/rfc/rfc6298).
- **Receiver**: per sender `low` (all seq < low delivered) + `W`-bit ring bitset for `[low, low+W)` → O(n·W) memory (FAQ Q34 reclamation). Deliver on first receipt (PL has no ordering), **always re-ack duplicates**.
- **Logger**: append lines into a large in-memory buffer; flush with one `write()` when it passes a threshold and at shutdown. Never write partial lines.
- **Shutdown**: on signal → set atomic `stop` → threads check it right after every `recv`/before every send and exit (recv uses a short timeout/poll) → join → final flush → exit.
- **Reuse for M2**: keep `PerfectLink` a module with `send(dest, payload)` + deliver callback, multi-destination ready, so URB/FIFO stack on top without rewrites.
- **Not doing** (unless measurements demand it): congestion control beyond window + RTO backoff, compression, lock-free structures, custom allocators.

## Phase 0 — Setup (Day 1, Oct 23)
- [ ] Build the untouched template: `cd template_cpp && ./build.sh`; run the 3-process manual example from §2.4.
- [ ] Verify netem on WSL loopback: `sudo tc qdisc add dev lo root netem delay 200ms 50ms loss 10% 25% reorder 25% 50%`, ping localhost, then `sudo tc qdisc del dev lo root`. Note: `tools/tc.py` pipes the VM password `dcl` to sudo → run its commands manually or pass your own password locally (don't commit it).
- [ ] Remove the `hello.c` demo: drop it from `src/CMakeLists.txt` `SOURCES` and the `hello()` call in `main.cpp`.
- [ ] Packaging script `tools/package.sh`: `cleanup.sh`, then zip the **contents** of `template_cpp/` (not the folder) into `submission.zip`; list the zip to check structure.
- [ ] Submit the trivial template to the Moodle **testing** milestone (test 0) once the link is open: confirms packaging + compiler.

## Phase 1 — Baseline, correctness first (Days 1–2)
- [ ] Parse config `m i` (receiver = i); read hosts via the template `Parser`; don't preallocate anything of size `m` (m can be MAX_INT).
- [ ] UDP socket + wire format encode/decode (bounds-check every received packet; drop malformed).
- [ ] Logger + signal/shutdown path (sigwait, stop flag, join, flush).
- [ ] Sender: window W (start 1024), 1 msg/packet, fixed RTO (e.g., 200 ms) — simplest correct version.
- [ ] Receiver: `low` + bitset dedup, deliver, ack every packet (incl. duplicates).
- [ ] Manual 3-process run with `example/` files; outputs look right.

## Phase 2 — Validation tooling (Day 2)
- [ ] `tools/validate_pl.py LOGSDIR [--crashed ids]` (or parse saved stress output for `Sending SIGTERM to process N`):
  - every line matches `^b \d+$` / `^d \d+ \d+$`, file ends with `\n`, size ≤ 64 MiB;
  - senders: `b` lines are exactly 1..k, no gaps/dups;
  - receiver: no duplicate `d` (PL2); every `d s q` has `b q` in sender s's log (PL3);
  - if receiver and sender s are both correct: all `b` of s delivered (PL1).
- [ ] `tools/run_stress.sh P M WAIT [tc]` = mkdir `runs/<ts>`, `(sleep WAIT; echo) | tools/stress.py perfect -r template_cpp/run.sh -l runs/<ts> -p P -m M | tee runs/<ts>/stress.log`, then validator.
- [ ] Optional dev build with ThreadSanitizer/ASan via a direct `g++ -fsanitize=… -Isrc/include …` script (cannot change the template CMake flags); run once on a stress test.

## Phase 3 — Correctness matrix (Days 2–3, re-run after every optimization)
- [ ] P ∈ {2, 3, 10, 50, 128} × M ∈ {10, 1 000, 100 000} without tc, no interference.
- [ ] Same with `tc` netem (delay/loss/reorder) on.
- [ ] Same with stress interference (STOP/CONT/TERM), incl. receiver being TERM'd (PL1 then not required).
- [ ] Edge cases: receiver started last / paused long (SIGSTOP), sender killed mid-batch, SIGTERM under max load (no partial lines), hosts with trailing spaces/newlines, M = 2147483647 (no huge allocation).
- [ ] Moodle testing milestone: run every test index (0..idx_max) via `test.txt`; all must pass before optimizing further.

## Phase 4 — Benchmark harness (Day 3)
- [ ] `tools/bench.sh P SECONDS`: hosts for P procs, config `<huge m> 1`, start all with `taskset -c 0,1` (2 cores like the grader), no tc, wait SECONDS (grader ≈ 2 min; use 30–60 s locally), SIGTERM all, wait for exit, report receiver `d` lines / SECONDS, output size, max RSS (`/usr/bin/time -v` or `/proc/<pid>/status`).
- [ ] Record baseline numbers for P ∈ {2, 10, 50, 128} in the log below.

## Phase 5 — Performance (Days 4–5; one change at a time, measure, keep only wins, re-run Phase 3 quick set)
- [ ] Batch 8 messages per DATA packet.
- [ ] Ack coalescing: cumulative ack + per-packet seqs; consider one ack per recv batch.
- [ ] `recvmmsg` / `sendmmsg` to cut syscalls.
- [ ] Tune W (possibly scaled by P to avoid receiver buffer overflow with 127 senders).
- [ ] Bigger `SO_RCVBUF`/`SO_SNDBUF` (check effective size with `getsockopt`).
- [ ] Adaptive RTO (replaces fixed) — mostly helps the tc/loss tests.
- [ ] Logger: hand-rolled int→text, large buffer, flush off the hot path if it shows up in profiles (`perf` if available).
- [ ] Check: no spinning (CPU of idle senders ≈ 0), threads ≤ 3–4, RSS small at P = 128.
- [ ] Watch output size: at very high throughput the receiver log can approach the 64 MiB cap — note numbers in the log.

## Phase 6 — Freeze & submit (Days 6–7, submit by Oct 29 evening, servers get slow near deadlines)
- [ ] Full Phase 3 matrix + all Moodle testing indices pass on the final build.
- [ ] Remove debug prints; stdout minimal.
- [ ] `tools/package.sh`; unzip into a temp dir, `./build.sh` there, run one stress test from the unzipped copy.
- [ ] Official **submitting** milestone: one attempt only, `submission.zip` without `test.txt`.
- [ ] Tag the commit (`m1-submitted`).

## Understanding checkpoints (final exam tests the project)
- [ ] Can explain: why stubborn links + dedup give PL1–PL3; why `b` is logged before sending; how the window + cumulative ack bounds memory; what happens on SIGSTOP/SIGTERM.

## Benchmark log
| Date | Change | P | msgs/s (receiver d-lines/s) | Output MiB | Max RSS | Notes |
|------|--------|---|-----------------------------|------------|---------|-------|

## Later (after M1)
- [ ] Milestone 2 FIFO-URB on top of the PL module (Dec 9). Milestone 3 Lattice Agreement (Dec 30).
- [ ] Recruiter-facing README once implementations exist.
