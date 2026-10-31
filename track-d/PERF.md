# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track D — Traverse

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 72.69 | 893 | 0 |
| 1 | Correct C baseline: CSR adjacency, forward Dijkstra per query (two-pass: rank-ordered targets). Byte-exact full ×3 — commit `92bd603` | (superseded) | ~5.97 | ~305 | **32.49** |
| 2 | Source compression — three bugs self-caught before push (loop counter `u`, `p = hdr` init, 32-bit `hv` type) — commit `7c1bcac` | (superseded) | — | — | **33.48** |
| 3 | 12 query workers: duplicate query lines folded before dispatch (first occurrence wins, open-addressing cache), workers pull query indices via `__atomic_fetch_add` counter, each thread owns dist/stamp/heap; answers written by query index, printed in query order after the join. Byte-exact full ×3 locally (2.0–2.4 s, was 6.1 s) — commit `01d434a` | **run-00a378773afa** | **1.279** | 305.0 | **43.20** |

Board facts for run-00a378773afa: all gates PASS · T=1.279 s vs ref 72.693 s = **57×** · K=7.31 · S=221 · M=305.0 MB · build 1.07 s.

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| A stale sample hash in our internal notes (`a4c76ed0441b…`) | The gate flagged a hash mismatch that was never a code bug — the real sample hash is `a4c76ed030bc…` | Notes corrected; the spec's published hashes are the only truth |
| Post-increment atomic for the work counter | The Track A incident (post-increment shifts every slot by one) was already documented, so the pull-counter here uses `__atomic_fetch_add` from the start — recorded as the lesson applied before it bit | Kept pre-increment |

## Notes on method

- Memory is the run cgroup's `memory.peak`; the per-thread dist (1.74M × 8 B ≈ 14 MB) and stamp arrays dominate M. A trim pass (fewer workers / narrower stamps) is in progress; T is sqrt-weighted against a 72.7 s reference, so a small T regression trades well against a big M win.
- Determinism: identical output hashes on 3 consecutive full runs before every push.
