# Native vector map widget: first slice

Status: design for the public `fynx-lab/CUI` fork's `feature/native-mvt-map` branch. Approved direction: CUI parses and draws a lightweight MVT map; the host provides tile data. No Vivida integration in this slice.

## Purpose and limits

Add a C99, cross-platform CUI widget that draws a pan/zoom map from Mapbox Vector Tiles (MVT 2.1) using CUI's own renderer. An optional Windows example loads real no-key OpenFreeMap vector tiles over HTTPS; the CUI library does not perform network I/O or depend on a particular provider. A deterministic, offline fixture exercises the same widget in gallery screenshots and tests.

The first slice displays a styled land background, water fills, road lines, basic settlement labels and supplied point markers. It supports pointer drag, wheel zoom, keyboard navigation, hit testing and both dark/light CUI themes. It does **not** reproduce all 47 layers of OpenFreeMap's current dark style: no sprites, glyph API, hill shading, buildings, line dashes, full MapLibre style expressions, label placement engine or tilted/globe view. These are future phases, not hidden requirements of the demo.

## External seam: a small, provider-neutral interface

The precise names are subject to a compile-and-test pass, but the semantics are fixed:

```c
/* Caller owns the camera; latitude/longitude are WGS84 degrees. */
typedef struct cui_map_view { double latitude, longitude; float zoom; } cui_map_view;
typedef struct cui_map_marker { int id; double latitude, longitude; const char *label; } cui_map_marker;
typedef struct cui_mvt_tile cui_mvt_tile;

/* Pure, no cui_ctx; accepts an uncompressed MVT protobuf payload. Null on bad input. */
cui_mvt_tile *cui_mvt_tile_decode(const void *bytes, size_t size);
void cui_mvt_tile_destroy(cui_mvt_tile *tile);

typedef const cui_mvt_tile *(*cui_map_tile_lookup)(void *user, int z, int x, int y);
typedef void (*cui_map_tile_request)(void *user, int z, int x, int y);
typedef struct cui_map_opts {
    int _;
    cui_size width, height;           /* zero values follow CUI defaults */
    cui_map_tile_lookup lookup;      /* nonblocking; null for a cache miss */
    cui_map_tile_request request;    /* enqueue a missing visible tile, never wait */
    void *tile_user;
    const cui_map_marker *markers;
    int marker_count;
    int min_zoom, max_zoom;          /* zero defaults to the widget's supported range */
} cui_map_opts;
typedef struct cui_map_result {
    bool view_changed;
    int hovered_marker, clicked_marker; /* -1 when none; caller-supplied ids otherwise */
} cui_map_result;
cui_map_result cui_map_ex(cui_ctx *ui, const char *id, cui_map_view *view,
                          const cui_map_opts *opts);
```

Follow CUI's `_ex` plus named-argument macro convention. The widget asks only for tiles it can show (plus at most a one-tile border during active movement); `lookup`/`request` run during the frame's build/input phase, never from painting or background threads. Callbacks must not block, and a missing tile may be requested more than once: the adapter deduplicates. A returned decoded tile and marker labels remain valid through `cui_end_frame`. The caller owns tiles, network jobs and caching; it may free tiles after frames that no longer reference them. The caller requests a redraw on its UI thread when new tiles arrive. CUI owns camera math, clipping, drawing and pointer/keyboard interaction; it does not retain private photo paths, store downloaded tiles or set network policy.

## Internal map rules

- Web Mercator, 256 logical pixels per tile, integer zoom for the first slice. Clamp latitude to ±85.05112878°, zoom to the supported range; wrap X across the antimeridian, clamp Y. Cursor-anchored wheel zoom and drag pan keep map points stable on screen. Points lacking valid finite coordinates are ignored.
- MVT decoder bounds all reads and counts and treats malformed, truncated, unsupported or over-limit input as an error instead of reading past buffers. It supports protobuf unknown-field skipping, layer extents, command/zigzag geometry, polygon ring order and holes. Avoid unbounded geometry, recursion or allocations proportional to adversarial input. No full-image decoding on the UI thread.
- Select a documented subset of OpenMapTiles-like layer names (`water`, `waterway`, `transportation`, `place`, optional `landcover`) with theme-derived colors. Keep style decisions inside the map module, not the network adapter. Triangulate concave fills with holes rather than feeding them directly to CUI's convex-only polygon primitive; otherwise reject a geometry safely instead of drawing an invalid fill.
- Only visible geometry and markers are drawn. Dense labels use simple overlap rejection and a fixed count budget. Marker id is the app-defined selection key; the widget is not coupled to EXIF or file browsing. Maintain stable draw output when nothing changes so software-renderer dirty-tile detection still works.

## Online demo adapter

`examples/map_online_win32.cpp` (Win32-only C++11 adapter; C99 core unchanged) fetches TileJSON from `https://tiles.openfreemap.org/planet`, reads the returned template instead of hardcoding its versioned PBF URL, then asynchronously fetches only visible requested tiles via WinHTTP. It identifies itself with an app-specific User-Agent, honors response failures/redirects/size and time limits, decompresses HTTP content as necessary before pure MVT decode, and maintains a small bounded in-memory cache. At most four requests may be in flight; no sweep, bulk prefetch or offline download. No API key, private paths or user photos are sent. If offline or throttled, show a clear failure/loading state and keep input responsive. Show `OpenFreeMap © OpenMapTiles Data from OpenStreetMap` visibly, unobscured, and document the service's no-SLA and terms at https://openfreemap.org/tos/ . The example is deliberately Windows-only; the widget and deterministic tests build on all CUI backends.

A separate offline gallery scene displays the same widget with tiny generated MVT tiles and fictional markers; it needs no network and supplies stable render snapshots. Demo marker clicks update a label; Vivida is unchanged.

## Verification and staged release

1. Capture baseline before edits (`cmake --build cui/build-vs18 --config Debug --parallel 8` passed; `ctest --test-dir cui/build-vs18 -C Debug --output-on-failure` reported 29/29 passing at upstream e2a0ad2). Preserve the existing tests.
2. Tests through the public seam cover tile decode, unknown/truncated/oversized PBF, geometry with holes, coordinates at poles and antimeridian, pan/zoom stability, keyboard and marker picking. Add gallery and component screenshots in light/dark and review Windows OpenGL/software rendering. Run CUI's C99 and amalgamated builds, CTest, and any available sanitizers. Distinguish untested OSes in the final report.
3. Build and run the Win32 demo against live OpenFreeMap with an offline/failure path. Check visible-only requests, cancellation/deduplication, texture/cache bounds and attribution. No claim of mainland connectivity from a non-mainland test machine.
4. Follow CUI's change rules: document the public API, add gallery scene and regenerated images, tests, `CHANGELOG.md` entry, and exact AI-model co-author trailer on commits. Keep experimental work on the fork feature branch; do not change Vivida's pinned `GIT_TAG`, patches, installer, settings or public release. Only push verified code to the fork, with no upstream PR or new release tag unless separately approved.

Primary sources: MVT geometry spec https://github.com/mapbox/vector-tile-spec/blob/master/2.1/README.md ; OpenMapTiles schema https://openmaptiles.org/docs/schema/ ; OpenFreeMap homepage and terms https://openfreemap.org/ and https://openfreemap.org/tos/ ; CUI contribution and architecture guides in this repository.
