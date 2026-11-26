# Theme-aware CUI map styling implementation plan

Approved spec: [CUI map theme and style](../specs/2026-11-26-cui-map-theme-style-design.md). No tests or window-opening programs may run without separate user permission; compile-only verification is allowed.

1. Define a compact C99 public palette and default-palette helper; pass an optional palette via map options and copy the effective colors into frame paint data. Add an opt-in styled base-tile drawing entry point; keep the existing one as the default-theme wrapper.
2. Replace scattered hard-coded map colors with one palette for both live map painting and base tile baking. Keep marker fill/text dynamic via CUI accent/foreground tokens and retain road widths and zoom filters.
3. Switch the Windows demo to initial system-theme following with a user-facing sun/moon toggle. Make the offscreen raster baker use the main theme and palette. Invalidate only raster images on theme or base-palette change, in addition to existing DPI invalidation; do not flush decoded tiles or disk MVT bytes.
4. Add offline test sources for palette defaults/overrides and theme-sensitive vector-versus-raster parity; document color customization and cache invalidation. Compile relevant targets without running tests or launching the demo; inspect diff and commit locally without pushing.
