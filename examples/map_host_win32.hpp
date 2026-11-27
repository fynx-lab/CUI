#pragma once
#include <cui/cui.h>

/* Experimental Windows host adapter built from the online example's proven
 * TileJSON/WinHTTP/cache engine. Core CUI remains transport-independent. */
struct map_host_win32;
enum class map_host_proxy_mode { System, Direct, Manual };
struct map_host_connection {
    map_host_proxy_mode mode;
    const wchar_t *manual_proxy; /* validated host:port; copied before returning */
};
map_host_win32 *map_host_create(cui_ctx *ui, void *native_window, const map_host_connection &connection);
// Call on the UI thread. Queued work is canceled; already-running requests remain bounded.
bool map_host_set_connection(map_host_win32 *host, const map_host_connection &connection);
void map_host_begin_frame(map_host_win32 *host);
void map_host_options(map_host_win32 *host, cui_map_opts *options);
void map_host_end_frame(map_host_win32 *host);
void map_host_pause(map_host_win32 *host);
void map_host_retry(map_host_win32 *host);
constexpr unsigned map_host_tile_body_too_large = 1001u; /* local failure, not HTTP */
struct map_host_tile_failure { int z, x, y; unsigned status; };
bool map_host_visible_failure(map_host_win32 *host, map_host_tile_failure *out);
void map_host_retry_tiles(map_host_win32 *host);
int map_host_status(const map_host_win32 *host); /* 0 loading, 1 ready, -1 error */
unsigned map_host_last_http(const map_host_win32 *host);
void map_host_destroy_after_app(map_host_win32 *host); /* call after cui_app_run */
