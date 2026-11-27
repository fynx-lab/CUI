# MVT layer metadata bounds without arbitrary table-count rejection

Date: 2026-11-27. Approved approach: count-then-allocate bounded metadata, not a larger fixed value-table cap.

## Confirmed failure

Experimental Vivida reported `Map tile failed (vector tile decode failed) z2/1/0`. The exact OpenFreeMap versioned tile returned HTTP 200, MVT content type and 1,094,686 uncompressed bytes, under the existing 2 MiB input limit. Its `place` layer carries 33,766 value fields; `src/cui_map.c` currently rejects the first value beyond `MAP_VALUES = 32768` in a required layer. The failed tile is absent from disk cache because the host writes only decoded tiles. Retrying re-fetches the same tile and encounters the same deterministic rejection. Other tiles at the zoomed-in level can visually cover the hole on zooming back, without proving the original tile succeeded.

## Chosen design

Keep the public C99 `cui_mvt_tile_decode` API and the 2 MiB uncompressed tile input limit unchanged. In `map_layer`, preserve its first pass over each layer to validate protobuf field boundaries, name, version and extent, but also **count all length-delimited feature, key and value fields**. For recognized layers, allocate one temporary contiguous slice table sized to exactly those three counts, with checked `size_t` addition/multiplication. Do not allocate for unknown layers. The second pass fills the tables and parses values, retaining existing tag-index validation and layer/feature semantics. Free the table before returning from that layer on both success and failure. Each metadata field uses at least a tag and a length byte, so total entries are bounded by half the already-capped 2 MiB tile size; with 16-byte slices the temporary table has an approximately 16 MiB worst-case ceiling per decoding worker, rather than an unbounded allocation. Allocation failure still rejects the tile safely.

Remove `MAP_FIELDS` and `MAP_VALUES` as rejection thresholds for the metadata tables only; do **not** relax the existing limits on decoded features, paths, points, geometry or polygon spans, or process unlimited optional labels. A tile exceeding those separate rendering/resource limits may still be rejected; this change prevents large *metadata tables* alone from invalidating an otherwise supported tile. No change to disk format, source hash, HTTP policy, CUI cache ownership, markers or Vivida's default pinned CUI dependency.

## Alternatives rejected

- Increase value/key/feature constants: simpler but merely moves the failure to another provider tile or version.
- Stream only the used name/class values: lower peak memory, but needs more passes and a new sparse index/tag resolution path; greater parser complexity and regression risk for this bounded 2 MiB input.

## Regression and verification

Add synthetic MVT fixtures in `tests/test_map.c`: a recognized `place` layer with at least 33,766 value entries referencing its final entry; a key table beyond 16,384 with the final name key referenced; and over 16,384 non-rendered feature envelopes followed by a valid rendered feature (without exceeding the existing 12,000 rendered-feature limit). Assert successful decoding and that name lookup still reaches the last indexed entries. Preserve existing tests for malformed tags, truncated protobuf and 2 MiB input rejection. Compile the CUI test target, online demo and experimental Vivida; per current preference, do not run tests or launch windows without separate authorization. Runtime success of the user's exact tile remains unverified until an authorized replay or user verification; if it still fails, investigate other decoder limits separately rather than claiming complete recovery.
