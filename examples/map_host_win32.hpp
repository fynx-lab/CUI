#pragma once
#include <cui/cui.h>

/* Experimental Windows host adapter built from the online example's proven
 * TileJSON/WinHTTP/cache engine. Core CUI remains transport-independent. */
struct map_host_win32;
map_host_win32 *map_host_create(cui_ctx *ui, void *native_window);
void map_host_begin_frame(map_host_win32 *host);
void map_host_options(map_host_win32 *host, cui_map_opts *options);
void map_host_end_frame(map_host_win32 *host);
void map_host_pause(map_host_win32 *host);
void map_host_retry(map_host_win32 *host);
unsigned map_host_failed_tile(map_host_win32 *host); /* 0 = no visible failure */
void map_host_retry_tiles(map_host_win32 *host);
int map_host_status(const map_host_win32 *host); /* 0 loading, 1 ready, -1 error */
unsigned map_host_last_http(const map_host_win32 *host);
void map_host_destroy_after_app(map_host_win32 *host); /* call after cui_app_run */
