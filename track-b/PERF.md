# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track B — Index

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 56.35 | 2,200 | 0 |
| 1 | First C engine: parse all query lines first and intern only their terms (the spec blesses this); 12-thread walk of line-aligned corpus ranges (a newline-count pre-pass fixes each range's first doc id); per-term per-thread posting runs merged into sorted arrays; terms over df 8,000 become N-bit bitmaps; BOOL evaluates left-to-right on a result bitmap (dense sides word-parallel); PHRASE walks the smallest posting list, filters by membership, then verifies candidates by re-tokenizing raw text for the consecutive sequence. Byte-exact sample + full ×3 (6.45/6.87/3.49 s local) — commit `987463a` | **run-7a4b13c4e707** | 1.263 | 255.3 | **45.21** |
| 2 | Two-bit bloom over the term hashes (512 KB, set at intern time) — most corpus tokens now skip the hash-probe entirely; a 256-entry class table folds A-Z and classifies each byte in one load. Byte-exact sample + full ×3 (4.19/3.49/3.28 s local, was 6.45/6.87/3.49) — commit `0edb573` | **run-93c8c9ddce5d** | **1.239** | 257.2 | **45.46** |

Board facts for run-93c8c9ddce5d: all gates PASS · K=8.11 · S=464 · M=257.2 MB · build 1.70 s.

Profiling note (phase clock inside a scratch build, sample): query-parse 0.023 s, newline count 0.003 s, corpus walk 0.226 s, merge 0.015 s, query phase 0.006 s — the walk is 83% of the run, which is why steps 2+ attack token classification and probe skipping.

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| First tokenizer: uppercase bytes treated as separators | `Habeas` tokenized as `abeas`, `U.S.C.` produced nothing, `Case` as `ase` — the fold existed only in the hash, not in the scan. Caught by the spec's own worked example (18 queries, exact expected bytes) before any board run | Fold A-Z → a-z at scan time; every doc token now matches its query term |
| Operator detector required exactly 3 letters | `OR` (2 letters) was interned as a term; every OR-containing BOOL query misparsed (e.g. `the OR writ OR case` became a 5-term AND chain) — worked example caught it | Length-2 `OR` added to the detector |
| First thread-runner passed one base pointer | All 12 walkers received range 0 — instant segfault at the gate | Per-thread stride in the runner |

## Notes on method

- The worked example in the spec (5 documents, 18 queries, exact `results.txt`) is the fastest correctness gate we found for this track — it caught both parse bugs in minutes, before Docker was ever involved.
- Postings are appended doc-ascending per thread, so the within-document dedup is just "last element == current doc"; ranges are disjoint so the k-way merge needs no dedup at all.
- Duplicate query lines are detected at parse (line-hash) and copy the first answer after the join.
- Determinism: identical output hashes on 3 consecutive full runs before every push.
