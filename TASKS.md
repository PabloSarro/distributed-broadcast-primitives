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

## Moodle testing checkpoints (official correctness feedback)
Testing Milestone 1 is open (Oct 16 → Oct 30, 23:59): unlimited submissions, one test per submission, pass/fail only, no performance score. 7 tests (0–6); 0–3 return fast, 4–6 can take longer. Upload `submission.zip` (contents of `template_cpp/`, after `cleanup.sh`) plus `test.txt` holding a single integer 0–6. "Setup script failed" = wrong zip structure. This complements the local checks (it doesn't replace them): it's the only run in the real grading environment, but gives no details on why a test fails.
- [x] **CP0** (end of Phase 0): bare template → test 0. Expect a fail (no output), but no "setup script failed" → packaging + compiler OK.
- [x] **CP1** (end of Phase 1, baseline passes local manual run): tests 0–6. Start 4–6 first since they are slow. Any fail → fix before Phase 5.
- [x] **CP2** (end of Phase 5, after the last kept optimization): tests 0–6 again; optimizations must not break correctness.
- [ ] **CP3** (Phase 6, the exact zip to be submitted officially): tests 0–6 on that same zip.
- [ ] Record each checkpoint's results (commit hash, test → 0/1) in the log at the bottom.

### Preparing the submission files (Claude does this; the user only uploads)
Same procedure for every checkpoint and for the official submission. Wrapped in `tools/package.sh <test>` once it exists (Phase 0). `zip` isn't installed, so Python's built-in `zipfile` module is used instead.
1. Commit state: working tree clean, note the commit hash (`git rev-parse --short HEAD`) for the checkpoint log.
2. Build check: `cd template_cpp && ./build.sh` must succeed with zero warnings.
3. Clean: `./cleanup.sh` (removes `bin/da_proc` and `target/`).
4. Zip the **contents** of `template_cpp/` (no top-level folder, no `.gitignore`), from inside `template_cpp/`:
   `python3 -m zipfile -c ../submission.zip bin build.sh cleanup.sh CMakeLists.txt run.sh src`
5. Verify the structure: `python3 -m zipfile -l ../submission.zip` must show `build.sh`, `run.sh`, `cleanup.sh`, `CMakeLists.txt`, `bin/…`, `src/…` at top level, and no `target/` or `bin/da_proc`.
6. Verify it builds from the zip: extract into a fresh temp dir (`python3 -m zipfile -e`), `chmod +x *.sh` (Python's extractor drops file permissions), `./build.sh`, and do a quick 3-process run.
7. Testing milestone only: `printf '%d\n' <test> > test.txt` in the repo root (single integer 0–6). Official submission: **no** `test.txt`.
8. Hand-off: `submission.zip` (+ `test.txt`) are stored in the parent folder:, `/home/pablo/MA3/DA/submission/`, reachable from Windows at `\\wsl.localhost\Ubuntu-22.04\home\pablo\MA3\DA\submission\`. Both are git-ignored. The user uploads them to Moodle and reports the 0/1 results.

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
- [x] Build the untouched template: `cd template_cpp && ./build.sh`; run the 3-process manual example from §2.4. (Upstream stores the `.sh`/`.py` scripts without the executable bit → made them executable, so the zip carries it too.)
- [x] Verify netem on WSL loopback: `sudo tc qdisc add dev lo root netem delay 200ms 50ms loss 10% 25% reorder 25% 50%`, ping localhost, then `sudo tc qdisc del dev lo root`. Note: `tools/tc.py` pipes the VM password `dcl` to sudo → run its commands manually or pass your own password locally (don't commit it).
- [x] Remove the `hello.c` demo: drop it from `src/CMakeLists.txt` `SOURCES` and the `hello()` call in `main.cpp`.
- [x] Packaging script `tools/package.sh <test>`: automates steps 2–7 of "Preparing the submission files" above.
- [x] Moodle checkpoint **CP0** (see above).

## Phase 1 — Baseline, correctness first (Days 1–2)
- [x] Parse config `m i` (receiver = i); read hosts via the template `Parser`; don't preallocate anything of size `m` (m can be MAX_INT).
- [x] UDP socket + wire format encode/decode (bounds-check every received packet; drop malformed).
- [x] Logger + signal/shutdown path (sigwait, stop flag, join, flush).
- [x] Sender: window W (start 1024), 1 msg/packet, fixed RTO (e.g., 200 ms) — simplest correct version.
- [x] Receiver: `low` + bitset dedup, deliver, ack every packet (incl. duplicates).
- [x] Manual 3-process run with `example/` files; outputs look right.

## Phase 2 — Validation tooling (Day 2)
- [x] `tools/validate_pl.py LOGSDIR [--crashed ids]` (or parse saved stress output for `Sending SIGTERM to process N`):
  - every line matches `^b \d+$` / `^d \d+ \d+$`, file ends with `\n`, size ≤ 64 MiB;
  - senders: `b` lines are exactly 1..k, no gaps/dups;
  - receiver: no duplicate `d` (PL2); every `d s q` has `b q` in sender s's log (PL3);
  - if receiver and sender s are both correct: all `b` of s delivered (PL1).
- [x] `tools/run_stress.sh P M WAIT [tc]` = mkdir `runs/<ts>`, `(sleep WAIT; echo) | tools/stress.py perfect -r template_cpp/run.sh -l runs/<ts> -p P -m M | tee runs/<ts>/stress.log`, then validator.
- [x] Optional dev build with ASan/UBSan via `tools/sanitize.sh` (direct `g++ -fsanitize=…`, template CMake untouched); clean on a 4-process stress run. ThreadSanitizer aborts at startup on this WSL kernel (ASLR mapping), so it is not used.

## Phase 3 — Correctness matrix (Days 2–3, re-run after every optimization)
- [ ] P ∈ {2, 3, 10, 50, 128} × M ∈ {10, 1 000, 100 000} without tc, no interference (`tools/run_plain.sh`). Done: all M ∈ {10, 1 000}; M = 100 000 for P ∈ {2, 3, 10, 50}. P = 128, M = 100 000 now done too (2026-10-26, 150 s wait): 12.7 M delivered, all validator checks OK; receiver log 140 MB, so only the 64 MiB cap is exceeded (TA: not a grading issue; checked with `validate_pl.py --ignore-size`).
- [ ] Same with `tc` netem (delay/loss/reorder) on. Done (OK): P = 3, M = 1000 plain and with stress interference (2026-10-24); P = 10, M = 1000 plain and with stress interference (2026-10-26, user's terminal). P = 50, M = 1000 plain and with stress interference (3 procs TERM'd) under netem: OK (2026-10-26, user's terminal). Frozen build, all OK (2026-10-27, user's terminal): P = 128 M = 1000 plain and stress (9 TERM'd), P = 3 M = 100 000 plain and stress, P = 10 M = 20 000 plain and stress (2 TERM'd). ASan/UBSan stress clean (P = 4 M = 3000, P = 10 M = 5000 incl. receiver TERM'd). Not run: P = 128 M = 100 000 under netem.
- [ ] Same with stress interference (STOP/CONT/TERM), incl. receiver being TERM'd (PL1 then not required). Done (all OK, `tools/run_stress.sh`): P ∈ {2, 3, 10} × M ∈ {10, 1 000, 100 000}, P = 50 × {10, 1 000}, P = 128 × {10, 1 000}; receiver TERM'd in P = 3, M = 10. P = 50 and P = 128 with M = 100 000 now done too (2026-10-26; P = 128: 2 procs TERM'd, 12.5 M delivered, OK with `--ignore-size`; receiver log over 64 MiB).
- [ ] Edge cases: receiver started last / paused long (SIGSTOP), sender killed mid-batch, SIGTERM under max load (no partial lines), hosts with trailing spaces/newlines, M = 2147483647 (no huge allocation). Done (OK, no `tc`): receiver started 6 s after the senders, hosts file with trailing spaces/tab/blank line/no final newline, SIGTERM under max load ×5 (no partial lines), M = 2147483647 (RSS ≈ 4 MB). Sender killed mid-run (P = 10, M = 20 000, sender 2 TERM'd after 7 728 b lines) and receiver paused 20 s with SIGSTOP (P = 3 and 10, M = 20 000): all OK (2026-10-26). Note: in the P = 3 kill test the sender had already finished, so only P = 10 is a true mid-run kill.
- [x] Moodle checkpoint **CP1** (tests 0–6) — all passed (2026-10-25); safe to optimize.

## Phase 4 — Benchmark harness (Day 3)
- [x] `tools/bench.sh P SECONDS`: hosts for P procs, config `<huge m> 1`, start all with `taskset -c 0,1` (2 cores like the grader), no tc, wait SECONDS (grader ≈ 2 min; use 30–60 s locally), SIGTERM all, wait for exit, report receiver `d` lines / SECONDS, output size, max RSS (`/usr/bin/time -v` or `/proc/<pid>/status`).
- [x] Record baseline numbers for P ∈ {2, 10, 50, 128} in the log below.

## Phase 5 — Performance (Days 4–5; one change at a time, measure, keep only wins, re-run Phase 3 quick set)
- [x] Batch 8 messages per DATA packet (ACK now lists the seqs of the packet it answers, so this also coalesces acks per packet).
- [x] Ack coalescing (done: one cumulative `low` + 64-bit mask ACK per sender per receive burst): cumulative ack + per-packet seqs; consider one ack per recv batch.
- [x] `recvmmsg` / `sendmmsg`: receiver done (burst of 64). Sender-side sendmmsg tried: no measurable gain within noise (4 P values × 3 interleaved runs) → reverted.
- [x] Tune W: window = 3200/(n-1) rounded down to a multiple of 32, clamped to [32, 256] (n=2 → 256, n=10 → 256, n=50 → 64, n≥100 → 32); ACK bitmap is window/32 words. Budget sweep (interleaved, 4 repeats): 3200 > 1600 at P=30/50, ≥6400 no clearer win.
- [ ] Bigger `SO_RCVBUF`/`SO_SNDBUF` (check effective size with `getsockopt`).
- [ ] Adaptive RTO (replaces fixed) — mostly helps the tc/loss tests. Not started: perf runs have no network faults; fixed 100 ms RTO was not clearly better than 500 ms under the old scheme.
- [x] Logger: hand-rolled int→text into a fixed buffer (flush at 64 KiB). Interleaved A/B: neutral to slightly better (P=2 +40 %, P=128 +30 %, others within noise). `perf` is not installed; flush off the hot path not needed.
- [x] Check: finished processes use ≈ 0 CPU (0.01–0.02 s per 10 s idle), 2 threads per process, RSS ≈ 4 MB (also at P = 128).
- [x] Output size: no guard needed. TA (Moodle forum, Valentin Porchet): if the log format is right, ignore the 64 MiB limit; a test that hits it only measures raw throughput. Never add debug lines to the log, never double-deliver.
- [x] Moodle checkpoint **CP2** (tests 0–6) — all passed (2026-10-26, template_cpp at 705160e).

## Phase 6 — Freeze & submit (Days 6–7, submit by Oct 29 evening, servers get slow near deadlines)
- [ ] Full Phase 3 matrix passes on the final build; Moodle checkpoint **CP3** (tests 0–6) on the exact zip to submit.
- [ ] Remove debug prints; stdout minimal.
- [ ] `tools/package.sh`; unzip into a temp dir, `./build.sh` there, run one stress test from the unzipped copy.
- [ ] Official **submitting** milestone: one attempt only, `submission.zip` without `test.txt`.
- [ ] Tag the commit (`m1-submitted`).

## Understanding checkpoints (final exam tests the project)
- [ ] Can explain: why stubborn links + dedup give PL1–PL3; why `b` is logged before sending; how the window + cumulative ack bounds memory; what happens on SIGSTOP/SIGTERM.

## Benchmark log
| Date | Change | P | msgs/s (receiver d-lines/s) | Output MiB | Max RSS | Notes |
|------|--------|---|-----------------------------|------------|---------|-------|
| 2026-10-25 | baseline (fc3c7c1) | 2 | 66 k | 14.1 (20 s) | 3 MiB | 20 s run |
| 2026-10-25 | baseline (fc3c7c1) | 10 | 97 k | 19.8 (20 s) | 4 MiB | 20 s run |
| 2026-10-25 | baseline (fc3c7c1) | 50 | 184 k | 57.5 (30 s) | 4 MiB | 30 s run |
| 2026-10-25 | baseline (fc3c7c1) | 128 | 87 k | 26.5 (30 s) | 4 MiB | 30 s run |
| 2026-10-25 | batch 8/packet + list ack | 2 | 225 k | 50.5 | 3–4 MiB | 20 s run; validator OK (P=3 M=1000, P=10 M=100000) |
| 2026-10-25 | batch 8/packet + list ack | 10 | 198 k | 41.1 | 3–4 MiB | 20 s run; validator OK (P=3 M=1000, P=10 M=100000) |
| 2026-10-25 | batch 8/packet + list ack | 50 | 230 k | 47.1 | 3–4 MiB | 20 s run; validator OK (P=3 M=1000, P=10 M=100000) |
| 2026-10-25 | batch 8/packet + list ack | 128 | 212 k | 43.8 | 3–4 MiB | 20 s run; validator OK (P=3 M=1000, P=10 M=100000) |
| 2026-10-25 | (A/B, medians of 3 × 10 s, interleaved) batch+list-ack | 2/10/50/128 | 124 k / 141 k / 260 k / 176 k | – | – | same-binary runs vary 2–8×, so only compare interleaved medians |
| 2026-10-25 | + recvmmsg/sendmmsg receiver + cumulative/bitmap ack | 2/10/50/128 | 75 k / 304 k / 707 k / 355 k | – | – | P=2 regressed (latency-bound: W/RTT); P≥10 2–3× better. Validator OK: plain P=10,50,128; stress P=3,5,10 |
| 2026-10-25 | + window scaled by P (budget 3200) | 2/10/50/128 | 524 k / 421 k / 323 k / 199 k | – | – | 3 × 10 s medians; P=2 ~5×. Validator OK: plain P=2,10,50,128; stress P=3,10 |
| 2026-10-25 | + hand-rolled logger | 2/10/50/128 | A/B medians vs previous: 308 k vs 222 k / 536 k vs 448 k / 920 k vs 940 k / 433 k vs 295 k | – | – | machine was faster this session; only the relative A/B is meaningful |

Findings so far (2026-10-25):
- Benchmark noise on WSL is huge (identical binary: 85 k–270 k msgs/s at P=2). Always compare interleaved A/B runs (`BIN=… tools/bench.sh`, `CPUS=…`), ≥3 repeats, use medians; `msgs_per_cpu_s` is a steadier secondary metric.
- Window W sweep (16/32/64/128) and RTO sweep (20/50/100/500 ms) with the old ack scheme: no clear winner within the noise; RTO 20 ms is clearly bad (retransmit storms at P=128). Kept W=32, RTO=500 ms, scan 50 ms.
- Receive-buffer drops (`/proc/net/snmp` RcvbufErrors) ≈ 6 k per 15 s run at P=128. `rmem_max` here is 212992, so SO_RCVBUF cannot be raised without privileges.
- Cost is per datagram (~40 µs CPU on WSL loopback): fewer datagrams (batching, ack coalescing) was the big win.
- P=2 is latency-bound (throughput ≈ W / RTT) → try window scaled up when there are few processes.

## Moodle checkpoint log
| Date | CP | Commit | Results (test: 0/1) | Notes |
|------|----|--------|---------------------|-------|
| 2026-10-24 | CP0 | 20cd420 + uncommitted Phase 0 changes | 0: 0 | Setup and build OK; failed only because no `proc0N.output` files exist (expected for the bare template). |
| 2026-10-25 | CP1 | fc3c7c1 (zip built 2026-10-24 17:15) | 0–6: all 1 | Feedback only says "No Error" per test; stored in `submission/CP1/`. |
| 2026-10-26 | CP2 | 705160e (template_cpp unchanged since c41b9f9) | 0–6: all 1 | Batching, coalesced bitmap ACKs, recvmmsg/sendmmsg receiver, window scaled by P, hand-rolled logger. Feedback in `submission/CP2/`. |

## Later (after M1)
- [ ] Milestone 2 FIFO-URB on top of the PL module (Dec 9). Milestone 3 Lattice Agreement (Dec 30).
- [ ] Recruiter-facing README once implementations exist.
