# Map marker pointer and hover feedback

Date: 2026-11-27. Approved in conversation.

## Interaction

The generic CUI map widget already determines `cui_map_result.hovered_marker` for a marker under the pointer. While a marker is hovered, set the CUI hand cursor and render that marker with a slightly larger accent circle and an opaque white border; restore its normal size immediately when the pointer leaves. The normal border uses a light, non-white color in both the light and dark default map palettes. Other markers and the non-marker map area keep their current appearance and cursor. Do not enlarge the clickable region inconsistently with the visible hover state. Do not add perpetual animation or alter marker click, cluster splitting, projection or map drag behavior.

## Ownership

Implement hover feedback inside `src/cui_map.c` using the existing hit-test result and paint data from the same frame, so the standalone CUI demo and opt-in Vivida host share behavior. No new public API, photo logic, cache, tile request, proxy policy or changes to the pinned CUI release dependency/patch. Respect both themes and host-supplied normal marker borders via the existing marker palette; the hovered border is white.

## Verification

Extend focused map tests to check cursor hand on a single marker and a cluster, cursor reset off-marker, and a visible pixel/geometry difference between normal and hover draw data. Compile CUI map test and Windows demo, default and experimental Vivida. Per user preference, do not execute tests or launch a window without separate permission. Runtime visual quality remains unverified until authorized.
