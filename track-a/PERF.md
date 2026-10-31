# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

Every row is a measurement we actually took, under the same contract the board uses
(digest-pinned images, sandboxed run, sha256-checked outputs). Board run IDs are in
the RUN ID column — judges verify them against the live board.

## Track A — Ingest

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 32.05 | 228 | 0 |
| 1 | Correct C baseline: single-threaded, mmap, interning, nearest-rank percentiles, dense group arena. Byte-exact sample `ca6311bb…` + full `495ca096…` ×3 locally (2.32/1.53/1.43 s) | **run-2b904c359cf2** | 0.776 | 42.2 | **44.43** |
| 2 | 12-thread line-range partition; per-thread intern pass 1; sequential merge in fixed thread order (deterministic ids); pass 2 re-reads ranges against the global table and fills group slices with `__atomic_fetch_add`; group sorts run in parallel off an atomic work counter. Byte-exact full ×3 locally at 0.55 s (was 1.46 s) — commit `ec3bd3d` | **run-bf78671cf234** | **0.119** | 43.6 | **62.98** |
| 3 | Source golf: thread tables folded into the range structs, one fire-and-join helper with per-thread stride. S 256→246; T ticked 0.119→0.122 — net −0.23 pts, kept for the S headroom. Byte-exact full ×3 (1.06/1.22/1.26 s local) — commit `213fb2f` | run-1b980fea7619 | 0.122 | 44.5 | 62.75 |

Board facts for run-bf78671cf234: all gates PASS · T=0.119 s vs ref 32.047 s = **269×** · K=18.18 · S=256 · D=1 · build 0.75 s.

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| `__atomic_add_fetch` for the fill slot | Returns the POST-increment value — every group's slice got written one slot too high, each group's last latency leaked into the next group. Deterministic but wrong hash ×3 at 0.62 s. Found by dumping one group's slice and seeing every percentile read one rank too high | `__atomic_fetch_add` (pre-increment). Full ×3 then byte-exact |
| Shared fire-and-join helper passing one base pointer | All 12 workers received range 0 — instant segfault at the first gate run | Helper takes a stride; each thread gets `(char*)args + t*stride` |
| -O2 vs -O3 as a UB probe | Both wrong, differently, during the fill-slot bug hunt — the signature that pointed at undefined behaviour rather than logic | Root cause was the atomic semantics above |

## Notes on method

- The three baseline bugs (fixed before step 1 ever scored): timestamp separator anchor `memchr(p+27,…)` read one byte early (ASan named a NULL+6 read; a Python replica of the parser confirmed the off-by-one); percentile rank used a 0-indexed nearest-rank into a 1-indexed rule (`-1` fix); the day rollover incremented inside the `mo == 10` branch, so Nov 1 never arrived.
- Local walls are for iteration only; the board's numbers are the ones that count.
- Determinism: every threading change must produce identical output hashes on 3 consecutive full runs before we push.
