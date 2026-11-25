# Online native map demo hardening

Status: proposed follow-up design for `fynx-lab/CUI:feature/native-mvt-map`. The user approved a Windows-only C++11 demo with vendored `json11`, while the provider-neutral CUI core stays C99. **This document is not an implementation claim.** Vivida and its pinned CUI dependency remain unchanged.

## Scope and order

Do the five agreed improvements in order, with a small verified commit at each step. First convert only the Win32 example from `.c` to `.cpp` and vendor the original MIT-licensed `json11.cpp`, `json11.hpp`, and `LICENSE.txt` from `dropbox/json11` commit `2df9473fb3605980db55ecddf34392a2e832ad35`. Build those sources only for the Win32 example with C++11; CUI's C99 library, offline gallery and amalgamation cannot depend on json11. Changing the example's language is a prerequisite, not a new rendering feature. No CMake network fetch at build time.

### 1. Viewport request lifecycle

Track tile keys `(z,x,y)` with a monotonically increasing request id; requests and results carry that id, so a late result cannot fill a recycled slot. Each frame's `lookup`/`request` visits the required tiles, including the widget's one-tile movement border; after the map call, cancel **queued** jobs no longer needed and free their pending slots. A worker checks cancellation before starting WinHTTP. Up to four already-active synchronous WinHTTP calls may finish normally; obsolete results are discarded rather than put into the cache. Never close an active WinHTTP handle from the UI thread. Retain request deduplication for keys that remain visible. Shutdown wakes and joins all workers without stranding slots or blocking indefinitely on an internal queue. Network timeouts remain finite.

Return per-request outcomes instead of a success boolean: success, HTTP status, timeout/transport, rejected response, invalid MVT, or canceled. Retry 429/503 according to a bounded `Retry-After` (seconds or HTTP date if supplied), and transient transport/5xx failures with capped exponential backoff and jitter (at most three automatic attempts per tile, no prefetch or immediate retry storm). Other 4xx/invalid data wait for an explicit user retry. The TileJSON request uses the same finite policy. Respect the provider's <=4 parallel offline-download guideline even though the demo downloads only visible tiles. Do not change the external CUI callback signatures.

### 2. Bounded decoded-memory cache

Add `size_t cui_mvt_tile_memory_usage(const cui_mvt_tile *)` to the provider-neutral C99 API: report the tile's owned protobuf copy plus current feature, path, point and span allocation capacities and its object, without accessing a `cui_ctx`. Include a unit test against input size and destroy/release behavior. The Win32 demo admits decoded tiles to a **96 MiB** ready-tile cache and still has at most 192 cache entries; it evicts LRU entries not referenced in the current frame. Do not evict a pointer already returned by `lookup` before `cui_end_frame`. Completed results use a separately bounded queue of at most eight items, workers use at most four uncompressed response buffers of <=2 MiB, and no worker silently drops a completion leaving a pending slot forever. If no space can be freed, keep the UI responsive, report a resource-limit state, and allow retry after moving the viewport or reducing zoom. Report both byte usage and hit/miss counts in a debug/status view; benchmark the real peak, rather than claiming the cache cap is the entire process-memory cap.

### 3. Loading and error UI

Distinguish TileJSON loading, visible tiles pending, partly loaded, offline/timeout, throttled, invalid tile, and cache-full states. Show an unobtrusive placeholder for each missing viewport tile, replacing it only when its decoded tile arrives; this needs a small, provider-independent CUI widget paint change (no HTTP code in CUI). Keep map interaction active while data loads, show a Retry action where appropriate, and preserve the always-visible OpenFreeMap/OpenMapTiles/OpenStreetMap attribution. Use the provider's `Retry-After` indication for throttle feedback. With no network, show a specific error rather than just an empty map; the `CUI_MAP_DEMO_OFFLINE=1` smoke path stays deterministic.

### 4. Legibility

Reserve marker labels before map place-name placement; suppress an identical nearby place name (the observed double `Berlin`). Measure Unicode text using CUI font metrics instead of `strlen * 7`, prioritize major settlements over minor ones and use the same screen-space collision list across tile boundaries. Tune road contrast/line widths by `class` and zoom without promising complete MapLibre expressions. Verify light/dark, small and large windows, 100% and high DPI, keyboard focus, marker selection and drag/wheel. Regenerate the offline gallery screenshots if widget pixels change.

### 5. Strict TileJSON and performance evidence

Replace `strstr`/`strchr` TileJSON handling with `json11::Json::parse`, reject a parse error/non-object/non-vector `tiles` array, and validate the first tile URL template as HTTPS, **exactly** `tiles.openfreemap.org`, an allowed `/planet/` path, and exactly one each `{z}`, `{x}`, `{y}` in supported order ending `.pbf`. Validate integer zoom bounds against the widget's supported 0–14 range, cap JSON to 64 KiB, retain redirect blocking and decompression and reject unexpected HTTP status or over-limit bodies. Tests feed reordered properties, escaped text, missing/wrong-type fields, hostile URLs, duplicate/malformed tokens and a valid live-shaped TileJSON fixture through this adapter, without reaching the network.

Add deterministic scheduler/cache/status tests (including rapid pan, repeated zoom, late completion after slot reuse, 429/503, offline and queue pressure). Record frame time/vertex count during a fixed offline tile/camera scene, and memory/hit/miss counts during a fixed live smoke; compare against the current example, not an unmeasured FPS promise. Build and run Win32 Debug and shaping CTest, C99 core-only and amalgamated syntax checks, and ASan on map tests when available. Visually inspect online and offline screenshots, distinguish automatic tests from manual interaction, and disclose that Linux/macOS online adapters and full MapLibre style support remain outside scope.

## Boundaries and release

No raster-tile fallback, bulk prefetch, offline area download, API key, EXIF/photo paths, Vivida dependency change, upstream PR or release tag. Follow CUI's documentation/gallery/changelog and exact AI co-author trailer requirements; push only verified changes to the fork feature branch. Public API addition for memory measurement is a reusable decoder statistic, not a provider- or transport-specific interface.
