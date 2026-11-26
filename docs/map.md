# Native vector map (experimental fork feature)

`cui_map_ex` draws a native, flat Web Mercator map from **uncompressed Mapbox Vector Tiles (MVT 2.1)**. It has no URL, provider, HTTP client, offline store or EXIF dependency. The caller owns the view and tile cache; CUI handles clipping, wheel and drag input, keyboard navigation and drawing. See the [offline gallery](../examples/gallery_more.c) and the Windows [live OpenFreeMap example](../examples/map_online_win32.cpp).

![Offline map fixture, light theme](images/components/native-map-light.png)
![Offline map fixture, dark theme](images/components/native-map-dark.png)

## Smallest host

```c
/* `cache_find` returns a decoded tile or NULL; `queue_visible_tile` never
   blocks and deduplicates outstanding requests. Host-owned view persists. */
cui_map_view view = {52.52, 13.405, 8};
/* In a frame, inside a layout scope: */
cui_map_result hit = cui_map(ui, "map", &view,
    .width = CUI_FILL, .height = CUI_PX(420),
    .lookup = cache_find, .request = queue_visible_tile, .tile_user = &cache,
    .markers = markers, .marker_count = marker_count, .max_zoom = 14);
if (hit.clicked_marker >= 0) select_marker(hit.clicked_marker);
```

Use `cui_mvt_tile_decode(data, length)` on uncompressed PBF data (a pure, bounded C99 function). It copies its input: release the downloaded bytes afterward, keep the returned tile until at least the end of the frame in which `lookup` returned it, then call `cui_mvt_tile_destroy`. Network/decompression, retry, request deduplication and cache eviction belong in the host. Keep marker labels and arrays valid through `cui_end_frame`; marker ids are nonnegative and `-1` means no hit. On a completed download, hand the parsed tile to the UI thread and request a redraw there. `lookup` and `request` execute synchronously during frame construction: **never wait for the network** in either callback. They are called only for visible tiles (with one extra tile of border while dragging).

Initial rendering supports theme-colored land and landcover, water polygons (including holes), transportation and waterway lines, a small budget of `place` names, and supplied markers. Zoom steps are integral from 0 to 14 by default; `min_zoom` and `max_zoom` override them. Pointer drag pans, wheel zooms around the pointer; focus the map and use arrow keys or `+`/`-` to navigate. During a drag, landcover detail, minor roads and waterways are temporarily omitted to reduce draw work; water, major roads and markers remain. Full detail returns on release. This does not alter which tiles are requested. All geometry and names originate in tile data; **this is not a full MapLibre style engine** (no expressions, imported fonts/glyphs, sprites, buildings, hill shading or language-specific label rules). Limit of 2 MiB uncompressed input and bounded feature/geometry allocations: rejected tiles return NULL. The example reports download failures separately and offers an explicit retry.

For the Windows online example, build `map_online_win32` with `CUI_BUILD_EXAMPLES=ON` and a C++11 compiler, then run it. The example (not the C99 core) vendors MIT-licensed [json11](../examples/vendor/json11/README.md) to validate OpenFreeMap's `https://tiles.openfreemap.org/planet` TileJSON and its current versioned `.pbf` template before issuing requests. WinHTTP workers fetch tiles (at most four requests at a time), with a RAM-only decoded cache limited to 96 MiB and 192 entries, a separate eight-result backpressure queue, and a visible `OpenFreeMap © OpenMapTiles Data from OpenStreetMap` attribution. These are cache limits, not a whole-process memory limit. Queued viewport work is dropped when obsolete; workers retry transient failures at most three times with capped backoff and `Retry-After` for 429/503; completed results for obsolete requests are ignored. A stopped demo wakes waiting workers; active requests are bounded by WinHTTP timeouts rather than forcibly closed from the UI thread. The map widget shows subdued placeholders until tile delivery, and the demo reports errors and cache usage. The Windows demo defaults to CUI's software renderer, without changing CUI's global default: on one Berlin z8 screenshot-mode probe, OpenGL peaked at 687–803 MiB process RSS, versus 134–135 MiB with software and the same ~664k draw vertices. These machine-specific measurements are **not** a process-memory guarantee or a frame-time benchmark. Set `CUI_MAP_DEMO_GL=1` to opt back into normal renderer selection. It has no API key or offline bulk mode. Set `CUI_MAP_DEMO_OFFLINE=1` to exercise the failure UI without fetching data, or `CUI_MAP_DEMO_SMALL=1` for a 460×360 window while checking responsive layout. Cache `H`/`M` are per-frame tile-lookup hit/miss counters, not unique network requests. The provider is external and has [usage terms](https://openfreemap.org/tos/) and no service-level guarantee; the CUI core does not take responsibility for connectivity, licensing or attribution. On other platforms, the reusable widget and offline gallery still compile; port the transport adapter to your platform as needed.
