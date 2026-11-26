# Theme-aware, application-owned CUI map styling

## Intent and reference

The supplied Larkspur Dashboard screenshots define the target: neutral, restrained surfaces in dark and light modes with a clear blue accent, not a saturated nautical map or a separate visual language. The tile provider supplies MVT geometries, classes and labels; it does not control map colors. CUI does not parse MapLibre style JSON. The C99 map widget stays provider-neutral and owns only theme-aware default styling and a compact host override interface.

## Defaults and application control

Expose a small `cui_map_palette` of solid RGBA colors for land, water, park/landcover, major roads, minor roads, place names, missing-tile background and marker halo. Supply a helper to obtain CUI's default palette for `CUI_THEME_LIGHT` or `CUI_THEME_DARK`; applications may copy and override fields and pass a palette in `cui_map_opts`. A null palette selects the current UI context's theme automatically. Default marker fill and marker-label color continue to use CUI accent and foreground tokens at draw time, so accent/theme transitions do not bake a marker into tile images. Applications can customize marker fill by changing CUI's accent; explicit per-map marker overrides are deferred. Road zoom thresholds, widths, labels' font/size/count, attribution and tile requests do not change.

Proposed restrained starting colors (subject to manual visual review, not a pixel match to the reference):

| Role | Dark | Light |
| --- | --- | --- |
| land | `#202326` | `#F4F5F4` |
| water | `#233844` | `#D7E8EE` |
| parks | `#2C3934` | `#E3EBE5` |
| major roads | `#8C979E` | `#B3BEC6` |
| minor roads | `#566169` | `#D4DBDF` |
| place text | `#E1E4E5` | `#273138` |
| placeholder | `#262A2D` | `#EDF0F1` |

Marker halo uses a quiet near-white/dark neutral appropriate to the theme. The blue marker fill inherits the active CUI accent (the Windows demo uses `CUI_ACCENT_BLUE`). Placeholder colors remain close to land, avoiding conspicuous flashing during cache warmup.

## Single source of truth for baked imagery

The vector fallback and host-baked base tiles must use the **same effective palette**. Add a palette-aware variant of `cui_map_tile_draw_base` while retaining the original function's default-theme behavior for existing callers. The map widget copies the effective palette into frame paint data; it never holds an application-owned pointer past the call. The Windows demo passes the effective palette to both map and offscreen baker. The raster cache is invalidated on CUI theme-mode change, DPI change, or a change to any color used in baked base tiles. A theme transition does not re-bake each animation step: select the target light/dark palette by mode, and let dynamic CUI marker/foreground colors animate normally. No dark image may survive into a light map or vice versa.

## Windows demo and verification

Set the demo to follow OS theme initially, rather than forcing dark. Add a small sun/moon toggle that calls `cui_set_theme` on the main context; a deliberate manual toggle stops OS-following until re-enabled (the demo need not add a separate setting). Match the offscreen baker's theme mode to the main context before drawing. Keep CJK fallback, OpenFreeMap attribution, 192 MiB decoded/96 MiB raster budgets and all network/disk cache policies unchanged. Disk MVT bytes remain style-independent; theme changes do not delete or re-download tiles. Add offline checks for default palette selection, application override, vector/baked-image parity and raster invalidation conditions where a non-window seam exists. Compile only by default; do not run tests or launch windows without separate permission, and disclose that the screenshot match has not been visually verified. No push without authorization.
