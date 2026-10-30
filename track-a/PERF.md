# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

Every row below is a measurement we actually took, on the same contract the board uses
(digest-pinned images, `--cpuset-cpus=0-11` where possible, warm page cache, 5 runs, middle-3 mean).
Board run IDs are the `RUN ID` column — judges verify them against the live board.

## Track A — Ingest

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 32.05 | 228 | 0 |
| 1 | Correct C baseline: single-threaded, mmap two-pass, dense latency arena, interning, nearest-rank percentiles. Byte-exact sample `ca6311bb…` + full `495ca096…` ×3 locally (2.32/1.53/1.43 s) | **run-2b904c359cf2** | **0.776** | **42.2** | **44.43** |

Board facts for run-2b904c359cf2 (2026-10-30T15:21:52Z): all 5 gates PASS (check/build/correctness/timeout/sandbox) · T=0.776 s vs ref 32.047 s = **41.3×** · M=42.2 MB · S=154 · D=1 · **K=7.74 → 44.43 pts**. Queued 3.3 s, build 1.59 s, image 117.5 MB.
Axes left on the table: T (single thread → 12-way split ≈ ×1.77 K), S (154 → ~62 ≈ +11%), M (42 → ~25 MB via streamed output ≈ +8%).
| 2 | 12-way range partition + line-aligned chunks | | | | |
| 3 | Byte-prefix hour indexing (no date parsing) | | | | |
| 4 | Endpoint interning (hash + exact compare) | | | | |
| 5 | Per-thread chunked latency arenas + merge | | | | |
| 6 | Percentile sort parallelised | | | | |
| 7 | Output buffering + tight formatting | | | | |

Failed experiments (still logged — they count):

| Step | Change | Result |
|---|---|---|
| — | (none yet) | — |

## Track C — Join

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 14.44 | 367 | 0 |
| 1 | Correct C baseline (single pass, byte-exact) | (pending) | | | |
| 2 | 12-way parse, region→country direct index | | | | |
| 3 | Per-country aggregates, no locks | | | | |
| 4 | Distinct-name fingerprinting (96-bit, collision floor ~1e-16) | | | | |
| 5 | Money/format fast paths | | | | |

## Track D — Traverse

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 72.69 | 893 | 0 |
| 1 | Correct C baseline (forward Dijkstra per query) | (pending) | | | |
| 2 | Parallel chunked parse + CSR build | | | | |
| 3 | 12 worker threads over queries | | | | |
| 4 | Bidirectional Dijkstra | | | | |
| 5 | Monster-query handling (settle-all) | | | | |

## Notes on method

- Memory is measured as the run cgroup's `memory.peak`; the harness prewarms `/data`, so we read in
  chunks and never map the inputs (mapping would charge the input pages to our cgroup).
- Times are wall clock, exec→exit, including container start and our whole pipeline (parse + build + queries).
- Local numbers are for iteration only; the board's numbers are the ones that count.
