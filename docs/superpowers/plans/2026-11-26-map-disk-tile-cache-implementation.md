# Map disk tile cache implementation plan

Approved design: [persistent tile cache spec](../specs/2026-11-26-map-disk-tile-cache-design.md). No tests or GUI programs may run without separate user permission; compiling is allowed.

1. Add a standalone Windows-only C++11 disk-cache module under `examples/`, with a per-user known-folder root, SHA-256 namespace of a validated template, safe bounded `z/x/y.pbf` paths, atomic tile and last-good TileJSON writes, length-bounded reads, explicitly touched timestamps, and graceful I/O failure. Include offline test source for namespace, corruption, restart and concurrent writes; compile only.
2. Route map tile workers through disk read/decode before WinHTTP; preserve existing request deduplication, bounded results, cancellation tokens, and 2 MiB tile limit. Persist only successfully decoded HTTP 200 tiles. Keep all file I/O off the UI thread.
3. Validate and persist TileJSON on the worker. Fall back to strictly revalidated last-good TileJSON only on network/transport failure, not malformed HTTP 200. Keep offline map usable after failed Retry; add an online-refresh action. On version change, discard old decoded/image entries and cancel/invalidate old jobs/results before admitting the new source.
4. Update UI indication, docs and changelog; compile the map demo and relevant test targets without running them. Inspect the diff and report any unverified runtime behavior; commit locally, never push without authorization.
