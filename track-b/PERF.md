# PERF — optimisation log

Live board: https://speeddemon.dev/board · Team **ZeroSouls** · branch `speed-demon` = submission branch.

## Track B — Index

| Step | Change | Run ID | T (s) | M (MB) | Points |
|---|---|---|---|---|---|
| 0 | Reference unchanged | — | 56.35 | 2,200 | 0 |
| 1 | First C engine: parse all query lines first and intern only their terms (the spec blesses this); 12-thread walk of line-aligned corpus ranges (newline-count pre-pass fixes each range's first doc id); per-term per-thread posting runs merged into sorted arrays; terms over df 8,000 become N-bit bitmaps; BOOL evaluates left-to-right on a result bitmap (dense sides word-parallel); PHRASE walks the smallest posting list, filters by membership, then verifies candidates by re-tokenizing raw text for the consecutive sequence. Byte-exact sample + full ×3 (6.45/6.87/3.49 s local) — commit `987463a` | **run-7a4b13c4e707** | 1.263 | 255.3 | **45.21** |
| 2 | Two-bit bloom over the term hashes (512 KB, set at intern time) — most corpus tokens skip the hash probe entirely; a 256-entry class table folds A-Z and classifies each byte in one load. Full ×3 local 4.19/3.49/3.28 s (was 6.45/6.87/3.49) — commit `0edb573` | **run-93c8c9ddce5d** | **1.239** | 257.2 | **45.46** |
| 3 | Per-thread Zipf token cache (prefix-keyed, full verification on hit) — commit `8b00c0f` | run at `8b00c0f` | — | — | 45.11 |
| 4 | Step 3 reverted: the cache scored below the bloom baseline (S cost of 30 code lines outweighed the hash savings). Back to the step-2 build — commit `350e114` | (best retained) | 1.206 | 255.6 | **45.78** (best after re-run settled) |
| 5 | Source golf: ternary op chains, merged guards — commit `24983aa` | (pending score) | — | — | — |

Board facts for the best run: all gates PASS · K=8.24 · S≈458 · M=255.6 MB.

Profiling note (phase clock, scratch build, sample): query-parse 0.023 s, newline count 0.003 s, corpus walk 0.226 s, merge 0.015 s, query phase 0.006 s — the walk is 83% of the run; steps 2+ attack token classification and probe skipping.

Failed experiments (they count):

| Attempt | What happened | Resolution |
|---|---|---|
| First tokenizer: uppercase bytes treated as separators | `Habeas` → `abeas`, `U.S.C.` produced nothing, `Case` → `ase` — the fold existed only in the hash, not in the scan. Caught by the spec's own worked example (18 queries, exact expected bytes) before any board run | Fold A-Z → a-z at scan time via the class table |
| Operator detector required exactly 3 letters | `OR` (2 letters) was interned as a term; every OR-containing BOOL misparsed (`the OR writ OR case` became a 5-term AND chain) | Length-2 `OR` added to the detector |
| Zipf token cache (step 3 above) | Board 45.11 vs 45.78 baseline — savings did not cover the S cost | Reverted with numbers |
| Shared fire-and-join passing one base pointer | All 12 walkers received range 0 — instant segfault at the gate | Per-thread stride |

Techniques used (20): query-first indexing of only queried terms (spec-blessed) · query-line interning at parse · duplicate-line detection (hash + length + memcmp, first answer reused) · newline-count pre-pass for doc-id arithmetic · line-aligned document ranges · 256-entry byte class table (fold + alnum in one load) · FNV-1a with case fold · two-bit bloom probe skip · per-thread posting runs with last-element within-doc dedup · k-way merge of disjoint sorted runs (no dedup) · dense-term bitmap conversion at df > 8,000 · word-parallel AND/OR/NOT on result bitmaps · sparse-side iteration for mixed-density ops · AND-rebuild through a second buffer · smallest-posting-list phrase candidates · membership by bitmap bit or binary search · raw-text re-tokenization for phrase verify (no position index) · sliding-window k-gram match · per-query answer slots + single-write output · absent-term semantics (AND empties; OR/NOT neutral).

Method note: the spec's worked example (5 documents, 18 queries, exact bytes) is the fastest correctness gate we found — it caught both parse bugs in minutes, before Docker was ever involved.
