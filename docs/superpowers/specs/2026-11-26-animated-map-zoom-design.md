# Animated map zoom with cross-zoom tile fallback

Date: 2026-11-26
Status: approved in conversation; implementation pending
Scope: native CUI map widget and its Windows online demo in `fynx-lab/CUI` only. Vivida, its pinned dependency/patch, and the eight-language site remain unchanged.

## Problem and desired behavior

Today the map rounds `view.zoom` to an integer, requests that level, and immediately draws placeholders for unloaded tiles. The demo's 48 MiB raster cache only returns images at the exact requested level. A wheel or keyboard zoom can therefore replace the whole visible map with blank placeholders before tiles arrive.

For each zoom, display the map continuously: animate from the currently displayed scale to the newest requested level; fill any missing destination area using already-cached imagery from the previous level; replace each area as its destination tile becomes ready. Never wait for the entire viewport. Temporary interpolation blur is acceptable, but no whole-viewport blanking when suitable cached imagery is available.

## Approach and alternatives

Use geographic, per-tile fallback, not a stretched whole-window screenshot. A screenshot is simpler but reveals its edges under pan/resize and cannot reliably replace one tile at a time. For zoom-in, the corresponding source-level parent image is cropped and magnified; for zoom-out, the corresponding source-level child images are shrunk into their target positions. Draw destination images over these source images as each arrives. Source and target pixels share the same world-coordinate transform, so wheel zoom stays anchored beneath the pointer. Existing decoded geometry can paint as a fallback where a cached raster does not exist, without starting network requests at the source level.

## State and rendering

- Retain the public `cui_map_view.zoom` as the **integer target** for compatibility, tile requests, and external controls. Keep animation state per map ID in the C99 widget: current visual zoom, start visual zoom, integer destination, elapsed/duration, and anchor world point and screen point. The animation lasts approximately 200 ms with a smooth ease-out and uses `cui_delta_time`/`cui_request_redraw`. `cui_theme_get(ui)->motion == 0` skips visual travel but **does not disable cached fallback**.
- Wheel zoom anchors at the pointer; keyboard `+`/`-` anchors at viewport center. New wheel/key input while moving retargets immediately from the currently visible fractional zoom, never queues discrete animations. Clamp targets to configured min/max and cap elapsed-time jumps. Drag during zoom adjusts the geographic center consistently; resize and DPI changes recompute screen positions from geographic coordinates, not from a frozen screenshot.
- Request only target-level tiles needed by the visible viewport (and the existing drag border); never prefetch a full zoom level. For every visible destination tile, paint a matching target raster if available, otherwise use decoded target vectors; for missing destination geometry use any available matching source-level cached raster/decoded tile, clipped to that destination area. If the animation spans more than one level, select the closest available cached level for that area. If none exists, keep the current placeholder. The old source remains useful after the 200 ms animation ends until replaced; animation completion is **not** an all-or-nothing swap.
- Place labels and markers remain live overlays and are never baked into the cached images. During a transition, labels appear only where the destination tile has decoded data; collision tests and marker positions use the displayed transform. A marker hit test must match its drawn position. Labels must not be duplicated from parent and child levels.
- The host owns tile/image lifetime through `cui_end_frame`. Extend the optional raster lookup/fallback contract only as needed; preserve default vector-only behavior and existing positional initializers. The Windows demo keeps old visible source images while still useful, evicts least-recently-used images when safe, and never exceeds its existing 48 MiB raster-image cap; its decoded cache stays at 96 MiB. At high DPI or under memory pressure, the fallback may be partial and must degrade safely to vectors/placeholders, rather than exceed either cap. Newly arrived target tiles are baked at most one per UI frame as today. Theme/DPI changes invalidate mismatched raster images; the demo theme is fixed dark. No new HTTP, fonts, EXIF, json11, or caching policy enters the C99 core.

## Failure and edge cases

- Slow, offline, failed, or delayed target tiles keep showing cached source pixels where available; retain the demo's existing retry/error UI and do not repeatedly request old levels. Temporary blur is expected, not a promise of identical styling at every zoom.
- Correctly wrap longitude and clip tile fragments at the antimeridian; clamp latitude and skip nonexistent polar tiles. Respect `MAP_REFS`, the 48 MiB raster cap, decoded/input limits, and normal offscreen texture lifetime. Zoom 0 and maximum zoom cannot be overshot.
- Avoid showing a previous geographic area after a large pan, resize, new center set externally, or fast repeated zoom: only use source tiles that actually intersect the current world-space viewport.

## Verification and acceptance

- Deterministic C99 map tests: world-coordinate alignment and pointer anchor over intermediate scales, zoom-in parent crop, zoom-out child composite, antimeridian and edge tiles, mid-animation retarget, keyboard anchor, reduced motion, marker hit testing and labels, no target data, and bounded reference counts. Host-side tests: source retention/eviction and 48 MiB cap including high DPI; missing-image/vector fallback and texture lifetime through frame completion.
- Live Windows screenshots/video at Beijing z7 and Berlin z8: slow/network-fault zoom in/out, rapid successive wheel inputs, pan while zooming, small window and DPI change. Compare with vector-only mode and check no whole-viewport blank when cached coverage exists. Record frame time and peak RSS; report any partial-coverage exception honestly. Re-run Debug/shaping/core-only builds and map tests. Do not claim a measured interactive FPS without actual input-to-display evidence.
