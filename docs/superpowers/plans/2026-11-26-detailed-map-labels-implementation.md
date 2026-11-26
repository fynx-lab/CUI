# Detailed map labels implementation plan

Approved spec: [detailed map labels](../specs/2026-11-26-detailed-map-labels-design.md). Compile only, no tests or GUI unless separately authorized.

1. Extend bounded C99 tile parsing to optional named point/line/polygon layers with explicit per-layer candidate caps. Preserve the existing decoder on base layers, roll back optional-layer allocations when its table bounds are exceeded and keep malformed admitted features invalid. Reuse existing geometry, do not add new transport or HTTP behavior.
2. Add stable anchor selection for points, lines and parks, then bounded priority-ordered label selection (place > road > water/landmarks > POI) above z12. Keep lower-zoom behavior and live marker/collision handling. Respect 256/three-per-cell at z13+, 192/two-per-cell below.
3. Add offline fixtures for z12/z13 and named point/line/polygon layers plus optional cap behavior; update map docs/changelog. Compile map test and Windows demo targets without running tests or launching a window; review and commit locally without pushing.
