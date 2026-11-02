# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

Every row is a measurement we actually took, under the same contract the board uses
(digest-pinned images, sandboxed run, sha256-checked outputs). Board run IDs are in
the RUN ID column — judges verify them against the live board. Board run-to-run
variance on unchanged code measured ±1.5% (Track D scored 44.75–46.03 on identical
bytes), so sub-0.5-point deltas below are treated as noise.

## Track A — Ingest

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 32.05 | 228 | 0 |
| 1 | Correct C baseline: single-threaded, mmap, interning, nearest-rank percentiles, dense group arena. Byte-exact sample `ca6311bb…` + full `495ca096…` ×3 locally (2.32/1.53/1.43 s) | **run-2b904c359cf2** | 0.776 | 42.2 | **44.43** |
| 2 | 12-thread line-range partition; per-thread intern pass 1; sequential merge in fixed thread order (deterministic ids); pass 2 re-reads ranges against the global table and fills group slices with `__atomic_fetch_add`; group sorts run in parallel off an atomic work counter. Full ×3 local 0.55 s (was 1.46 s) — commit `ec3bd3d` | **run-bf78671cf234** | **0.119** | 43.6 | **62.98** |
| 3 | Source golf: thread tables folded into range structs, one fire-and-join helper with per-thread stride. S 256→246; T 0.119→0.122 — net −0.23 pts (noise-range), kept for S headroom — commit `213fb2f` | run-1b980fea7619 | 0.122 | 44.5 | 62.75 |
| 4 | Parallel output formatting at precomputed offsets (serial digit-length pass, then 12 workers writing disjoint row slices) — commit `dddd70e` | run at `dddd70e` | ~0.135 | 44.6 | 60.28 |
| 5 | Step 4 reverted: the offset/length pass costs more than parallel formatting saves at this scale. Back to the step-2 build — commit `b05126c` | (best retained) | 0.119 | 43.6 | **62.98** |

Board facts for run-bf78671cf234: all gates PASS · T=0.119 s vs ref 32.047 s = **269×** · K=18.18 · S=256 · D=1.

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| `__atomic_add_fetch` for the fill slot | Returns the POST-increment value — every group's slice written one slot too high, last latency leaked into the next group. Deterministic but wrong hash ×3 | `__atomic_fetch_add` (pre-increment); full ×3 then byte-exact |
| Shared fire-and-join helper passing one base pointer | All 12 workers received range 0 — instant segfault at the first gate run | Helper takes a stride; each thread gets `args + t*stride` |
| Parallel output formatting (step 4 above) | Board-measured regression 62.98→60.28: the serial per-row digit-length pass outweighed the parallel write | Reverted with numbers; kept in history as the honest cost of the experiment |
| -O2 vs -O3 as a UB probe | Both wrong, differently, during the fill-slot hunt — the signature that pointed at semantics, not logic | Root cause was the atomic pre/post choice |

Techniques used (15): mmap zero-copy input · fixed-offset byte parsing (no date library) · memchr-anchored field scans · FNV-1a interning · dense `(hour,endpoint)` key indexing, no runtime hash maps · line-aligned 12-thread range partition · per-thread intern tables · fixed-order sequential merge for deterministic ids · atomic pre-increment fill counters · count-first allocation with prefix offsets · parallel sorts via atomic work counter · nearest-rank percentiles from sorted multisets · preformatted hour strings · single-write output buffering · ASCII fold in the hash.

Baseline bugs fixed before step 1 ever scored: timestamp anchor `memchr(p+27,…)` read one byte early (ASan NULL+6; Python replica confirmed); percentile rank off-by-one into a 1-indexed rule; day-increment inside the `mo == 10` branch so Nov 1 never arrived.
