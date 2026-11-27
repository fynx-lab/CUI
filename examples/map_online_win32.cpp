/* OpenFreeMap is a data source, not a CUI dependency. This Win32 example
 * fetches TileJSON and only visible MVT tiles, with a bounded RAM cache. */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winhttp.h>
#include <cui/cui.h>
#include <cui/cui_app.h>
#include <cui/cui_render.h>
#include "map_tilejson.hpp"
#include "map_demo_jobs.hpp"
#include "map_disk_cache.hpp"
#include "map_host_win32.hpp"
#include <new>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 384
#define QUEUE 64
#define RESULT_QUEUE 8
#define CACHE_BUDGET (192u * 1024u * 1024u)
#define RASTER_BUDGET (96u * 1024u * 1024u)
#define WORKERS 4
#define BODY_CAP (2 * 1024 * 1024)
typedef map_demo_slot slot;
typedef map_demo_job job;
typedef struct { int z, x, y, ok; DWORD status; unsigned retry_after_ms; uint64_t token; char *json; cui_mvt_tile *tile; bool offline, cached_tile; uint64_t network_epoch; } result;
typedef struct {
    CRITICAL_SECTION lock;
    HANDLE available, stop_event, threads[WORKERS];
    HWND window; bool stopping, paused, offline, refreshing;
#ifdef CUI_MAP_HOST_LIBRARY
    map_host_proxy_mode network_mode;
    wchar_t manual_proxy[260];
    uint64_t network_epoch;
#endif
    map_disk_cache *disk;
    job jobs[QUEUE]; int jhead, jtail, jcount;
    result results[RESULT_QUEUE]; int rhead, rtail, rcount;
    slot tiles[SLOTS]; map_demo_raster_slot images[SLOTS]; unsigned frame; size_t cache_bytes;
    cui_ctx *ui, *raster_ui; cui_sw_renderer *raster_sw;
    unsigned char *raster_pixels; size_t raster_capacity, raster_bytes, raster_tile_bytes;
    float raster_scale; bool raster_failed, raster_palette_valid;
    cui_theme_mode raster_mode;
    cui_map_palette raster_palette;
    char template_path[240]; int min_zoom, max_zoom, tilejson; int failures, selected;
    int pending, loaded; unsigned long long hits, misses, raster_hits, disk_hits, network_hits; DWORD last_status; uint64_t next_token;
    cui_map_view view;
} demo;

static bool queue_job(demo *d, job item) {
    bool queued = false;
    EnterCriticalSection(&d->lock);
    if (d->jcount < QUEUE && !d->stopping) {
        d->jobs[d->jtail] = item; d->jtail = (d->jtail + 1) % QUEUE; d->jcount++;
        queued = true;
        SetEvent(d->available);
    }
    LeaveCriticalSection(&d->lock);
    return queued;
}
static int fetch(const char *path, char **out, int cap, DWORD *status, unsigned *retry_after_ms,
                 DWORD access, const wchar_t *proxy) {
    int n = -1; *out = NULL; *status = 0; *retry_after_ms = 0;
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 256)) return -1;
#ifdef CUI_MAP_HOST_LIBRARY
    const wchar_t *agent = L"Vivida-Photo-Map-Experiment/0.1 (+https://vivida.fynxlab.com/)";
#else
    const wchar_t *agent = L"CUI-Map-Demo/0.1 (+https://github.com/fynx-lab/CUI)";
#endif
    if (access == WINHTTP_ACCESS_TYPE_NAMED_PROXY && (!proxy || !*proxy)) return -1;
    HINTERNET session = WinHttpOpen(agent, access,
                                    access == WINHTTP_ACCESS_TYPE_NAMED_PROXY ? proxy : WINHTTP_NO_PROXY_NAME,
                                    WINHTTP_NO_PROXY_BYPASS, 0);
    HINTERNET connect = NULL, request = NULL;
    if (!session) return -1;
    WinHttpSetTimeouts(session, 5000, 5000, 10000, 10000);
    connect = WinHttpConnect(session, L"tiles.openfreemap.org", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!connect) goto done;
    request = WinHttpOpenRequest(connect, L"GET", wide, NULL, WINHTTP_NO_REFERER,
                                 WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE);
    if (!request) goto done;
    DWORD flags = WINHTTP_DECOMPRESSION_FLAG_GZIP | WINHTTP_DECOMPRESSION_FLAG_DEFLATE;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DECOMPRESSION, &flags, sizeof flags)) goto done;
    flags = WINHTTP_DISABLE_REDIRECTS;
    if (!WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &flags, sizeof flags)) goto done;
    if (!WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        !WinHttpReceiveResponse(request, NULL)) goto done;
    DWORD code = 0, len = sizeof code;
    if (!WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &code, &len, WINHTTP_NO_HEADER_INDEX)) goto done;
    *status = code;
    if (code != 200) {
        if (code == 429 || code == 503) {
            wchar_t value[128] = {}; len = sizeof value;
            if (WinHttpQueryHeaders(request, WINHTTP_QUERY_CUSTOM, L"Retry-After", value, &len,
                                     WINHTTP_NO_HEADER_INDEX)) {
                wchar_t *end = NULL;
                unsigned long seconds = wcstoul(value, &end, 10);
                if (end != value && (*end == 0 || *end == L' ')) {
                    *retry_after_ms = seconds >= 30 ? 30000 : (unsigned)seconds * 1000;
                } else {
                    SYSTEMTIME date = {}; FILETIME date_file = {}, now = {};
                    if (WinHttpTimeToSystemTime(value, &date) && SystemTimeToFileTime(&date, &date_file)) {
                        GetSystemTimeAsFileTime(&now);
                        ULARGE_INTEGER target = {}, current = {};
                        target.LowPart = date_file.dwLowDateTime; target.HighPart = date_file.dwHighDateTime;
                        current.LowPart = now.dwLowDateTime; current.HighPart = now.dwHighDateTime;
                        if (target.QuadPart > current.QuadPart) {
                            unsigned long long ms = (target.QuadPart - current.QuadPart) / 10000;
                            *retry_after_ms = ms > 30000 ? 30000 : (unsigned)ms;
                        }
                    }
                }
            }
        }
        goto done;
    }
    char *bytes = static_cast<char *>(malloc((size_t)cap + 1));
    if (!bytes) goto done;
    n = 0;
    for (;;) {
        DWORD read = 0;
        if (!WinHttpReadData(request, bytes + n, (DWORD)(cap + 1 - n), &read)) { n = -1; break; }
        if (!read) break;
        n += (int)read;
        if (n > cap) { n = -2; break; }
    }
    if (n > 0) { bytes[n] = 0; *out = bytes; }
    else free(bytes);
done:
    /* A 200 header is not a complete body: retry interrupted/empty reads as
     * transport failures. A rejected over-limit body must not be retried. */
    if (n == -1 && *status == 200) *status = 0;
    if (n == -2 && cap == BODY_CAP) *status = map_host_tile_body_too_large;
    if (request) WinHttpCloseHandle(request);
    if (connect) WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);
    return n;
}
static DWORD WINAPI worker(void *arg) {
    demo *d = static_cast<demo *>(arg);
    for (;;) {
        WaitForSingleObject(d->available, INFINITE);
        EnterCriticalSection(&d->lock);
        if (d->stopping) { LeaveCriticalSection(&d->lock); return 0; }
        if (!d->jcount) { ResetEvent(d->available); LeaveCriticalSection(&d->lock); continue; }
        job j = d->jobs[d->jhead]; d->jhead = (d->jhead + 1) % QUEUE; d->jcount--;
#ifdef CUI_MAP_HOST_LIBRARY
        map_host_proxy_mode mode = d->network_mode;
        wchar_t proxy[260];
        memcpy(proxy, d->manual_proxy, sizeof proxy);
        uint64_t epoch = d->network_epoch;
#endif
        if (!d->jcount) ResetEvent(d->available);
        LeaveCriticalSection(&d->lock);
        char *bytes = NULL;
        DWORD status = 0; unsigned retry_after_ms = 0;
        int n = -1;
        /* File work and decoding are kept on this worker; the UI only admits
         * the completed tile. Fault probes bypass the disk cache as well. */
        const char *fault = j.z >= 0 ? getenv("CUI_MAP_DEMO_TILE_FAULT") : NULL;
        if (!fault && j.z >= 0 && j.z != (getenv("CUI_MAP_DEMO_BEIJING") ? 7 : 8) &&
            getenv("CUI_MAP_DEMO_ZOOM_FAULT")) fault = "404";
        bool forced_fault = fault && (!strcmp(fault, "404") || !strcmp(fault, "503") || !strcmp(fault, "0"));
        cui_mvt_tile *disk_tile = NULL;
        if (j.z >= 0 && !forced_fault && d->disk) {
            std::string cached;
            if (d->disk->read_tile(j.tile_template, j.z, j.x, j.y, cached)) {
                disk_tile = cui_mvt_tile_decode(cached.data(), cached.size());
                if (disk_tile) d->disk->touch_tile(j.tile_template, j.z, j.x, j.y);
                else d->disk->remove_tile(j.tile_template, j.z, j.x, j.y);
            }
        }
        /* The host must not look permanently stuck on a cold metadata fetch:
         * one bounded request, then an explicit error/retry. Tile jobs retain
         * the demo's existing three-attempt policy. */
#ifdef CUI_MAP_HOST_LIBRARY
        const int attempts = j.z < 0 ? 1 : 3;
#else
        const int attempts = 3;
#endif
        if (!disk_tile) for (int attempt = 1; attempt <= attempts; attempt++) {
            EnterCriticalSection(&d->lock);
            bool suspended = d->stopping || d->paused;
            LeaveCriticalSection(&d->lock);
            if (suspended) break; /* no new HTTP attempt after leaving the map */
            if (forced_fault) {
                n = -1; status = (DWORD)atoi(fault); retry_after_ms = 0;
            } else {
#ifdef CUI_MAP_HOST_LIBRARY
                DWORD access = mode == map_host_proxy_mode::Direct ? WINHTTP_ACCESS_TYPE_NO_PROXY :
                               mode == map_host_proxy_mode::Manual ? WINHTTP_ACCESS_TYPE_NAMED_PROXY :
                                                                     WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY;
                n = fetch(j.path, &bytes, j.z < 0 ? 65536 : BODY_CAP, &status, &retry_after_ms,
                          access, mode == map_host_proxy_mode::Manual ? proxy : NULL);
#else
                n = fetch(j.path, &bytes, j.z < 0 ? 65536 : BODY_CAP, &status, &retry_after_ms,
                          WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, NULL);
#endif
            }
            if (n > 0 || status == 200 || status == 507 || status == 508 || status == map_host_tile_body_too_large ||
                (status && status != 429 && status != 503 && status < 500) || attempt == attempts) break;
            unsigned delay = map_demo_retry_delay_ms(attempt, retry_after_ms,
                                                      GetTickCount() ^ (unsigned)j.token);
            if (WaitForSingleObject(d->stop_event, delay) != WAIT_TIMEOUT) break;
        }
        result r = {j.z, j.x, j.y, n > 0 || disk_tile != NULL, status, retry_after_ms, j.token, NULL, NULL, false, disk_tile != NULL, 0};
#ifdef CUI_MAP_HOST_LIBRARY
        r.network_epoch = epoch;
#endif
        if (j.z < 0) {
            map_tilejson parsed; std::string error;
            bool valid_network = n > 0 && map_parse_tilejson(bytes, parsed, error);
            if (valid_network) {
                if (d->disk) {
#ifdef CUI_MAP_HOST_LIBRARY
                    EnterCriticalSection(&d->lock);
                    if (epoch == d->network_epoch && !d->paused && !d->stopping)
                        d->disk->write_tilejson(bytes, (size_t)n);
                    LeaveCriticalSection(&d->lock);
#else
                    d->disk->write_tilejson(bytes, (size_t)n);
#endif
                }
                r.json = bytes;
            } else {
                free(bytes);
                if (!getenv("CUI_MAP_DEMO_OFFLINE") &&
                    (status == 0 || status == 429 || status >= 500) && d->disk) {
                    std::string saved;
                    if (d->disk->read_tilejson(saved) && map_parse_tilejson(saved, parsed, error)) {
                        r.json = static_cast<char *>(malloc(saved.size() + 1));
                        if (r.json) { memcpy(r.json, saved.data(), saved.size()); r.json[saved.size()] = 0; r.offline = true; }
                    }
                }
            }
            r.ok = r.json != NULL;
        } else {
            r.tile = disk_tile;
            if (!r.tile && n > 0) {
                r.tile = cui_mvt_tile_decode(bytes, (size_t)n);
                if (r.tile && status == 200 && d->disk) {
#ifdef CUI_MAP_HOST_LIBRARY
                    EnterCriticalSection(&d->lock);
                    if (epoch == d->network_epoch && !d->paused && !d->stopping)
                        d->disk->write_tile(j.tile_template, j.z, j.x, j.y, bytes, (size_t)n);
                    LeaveCriticalSection(&d->lock);
#else
                    d->disk->write_tile(j.tile_template, j.z, j.x, j.y, bytes, (size_t)n);
#endif
                }
            }
            r.ok = r.tile != NULL;
            free(bytes);
        }
        /* A slow UI frame can finish more than 68 jobs before it drains the
         * result queue. Never drop a result and strand its slot as pending. */
        for (;;) {
            EnterCriticalSection(&d->lock);
            if (d->stopping || d->paused) {
                LeaveCriticalSection(&d->lock);
                free(r.json); cui_mvt_tile_destroy(r.tile);
                break;
            }
            if (d->rcount < RESULT_QUEUE) {
                d->results[d->rtail] = r; d->rtail = (d->rtail + 1) % RESULT_QUEUE; d->rcount++;
                PostMessageW(d->window, WM_APP + 18, 0, 0);
                LeaveCriticalSection(&d->lock);
                break;
            }
            LeaveCriticalSection(&d->lock);
            PostMessageW(d->window, WM_APP + 18, 0, 0);
            Sleep(5);
        }
    }
}
/* Only accept a versioned tile path at OpenFreeMap's HTTPS host. TileJSON
 * supplies this path, never a compiled-in, potentially stale timestamp. */
static bool template_from_json(demo *d, const std::string &json) {
    map_tilejson parsed; std::string error;
    if (!map_parse_tilejson(json, parsed, error)) return false;
    memcpy(d->template_path, parsed.path_template.c_str(), parsed.path_template.size() + 1);
    d->min_zoom = parsed.min_zoom; d->max_zoom = parsed.max_zoom;
    return true;
}
/* Only the UI thread mutates cache entries; returned lookup pointers remain
 * owned through the end of their frame. */
static void install_local_cjk_fallback(cui_ctx *ui) {
    wchar_t windows[MAX_PATH];
    UINT n = GetWindowsDirectoryW(windows, MAX_PATH);
    if (!n || n >= MAX_PATH - 24) return;
    const wchar_t *fonts[] = { L"\\Fonts\\msyh.ttc", L"\\Fonts\\simhei.ttf" };
    for (int i = 0; i < 2; i++) {
        wchar_t path[MAX_PATH];
        wcscpy_s(path, MAX_PATH, windows);
        wcscat_s(path, MAX_PATH, fonts[i]);
        char utf8[MAX_PATH * 4];
        if (!WideCharToMultiByte(CP_UTF8, 0, path, -1, utf8, sizeof utf8, NULL, NULL)) continue;
        cui_font font = cui_font_load_file(ui, utf8);
        if (font >= 0) { cui_font_add_fallback(ui, CUI_FONT_BODY, font); break; }
    }
}
static map_demo_raster_slot *raster_find(demo *d, int z, int x, int y) {
    for (int i = 0; i < SLOTS; i++) {
        map_demo_raster_slot *s = &d->images[i];
        if (s->raster && s->z == z && s->x == x && s->y == y) return s;
    }
    return NULL;
}
static void raster_drop(demo *d, map_demo_raster_slot *s) {
    if (!s->raster) return;
    cui_image_destroy(d->ui, s->raster);
    d->raster_bytes -= d->raster_tile_bytes;
    *s = map_demo_raster_slot{};
}
/* After the previous frame is presented, source-level images can make room
 * for the target level even if the zoom fallback just displayed them. */
static int raster_source_victim(const demo *d, int target_zoom) {
    int oldest = -1;
    for (int i = 0; i < SLOTS; i++) {
        if (!d->images[i].raster || d->images[i].z == target_zoom) continue;
        if (oldest < 0 || d->images[i].raster_used < d->images[oldest].raster_used) oldest = i;
    }
    return oldest;
}
/* Only called between frames: textures returned to the map remain alive until
 * cui_end_frame, even when decoded data is evicted or a tile is replaced. */
static void raster_invalidate(demo *d) {
    for (int i = 0; i < SLOTS; i++) raster_drop(d, &d->images[i]);
}
static cui_texture raster_lookup(void *user, int z, int x, int y) {
    demo *d = static_cast<demo *>(user);
    map_demo_raster_slot *s = raster_find(d, z, x, y);
    if (!s) return 0;
    s->raster_used = d->frame; d->raster_hits++;
    return s->raster;
}
static bool raster_prepare(demo *d) {
    if (!d->raster_ui || !d->raster_sw) return false;
    float scale = cui_dpi_scale(d->ui);
    if (scale <= 0 || scale > 4) return false;
    int pixels = (int)(256 * scale + 0.5f);
    size_t bytes = (size_t)pixels * pixels * 4;
    if (bytes > RASTER_BUDGET) return false;
    cui_theme_mode mode = cui_get_theme_mode(d->ui);
    cui_map_palette palette = cui_map_palette_default(mode);
    /* Only base colors are baked. Place text, placeholders and markers stay
     * live, but any base-color or mode change invalidates old tile imagery. */
    if (!d->raster_palette_valid || d->raster_scale != scale || d->raster_mode != mode ||
        memcmp(&d->raster_palette.land, &palette.land, 5 * sizeof(cui_color))) {
        raster_invalidate(d);
        d->raster_scale = scale; d->raster_tile_bytes = bytes;
        d->raster_mode = mode; d->raster_palette = palette; d->raster_palette_valid = true;
        cui_set_theme(d->raster_ui, mode);
        cui_set_viewport(d->raster_ui, 256, 256, scale);
    }
    int selected = -1;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &d->tiles[i];
        if (s->state == 2 && s->tile && !raster_find(d, s->z, s->x, s->y) && s->used == d->frame - 1) {
            selected = i; break;
        }
    }
    if (selected < 0) return false;
    while (d->raster_bytes > RASTER_BUDGET - bytes) {
        int victim = map_demo_raster_victim(d->images, SLOTS, d->frame - 1);
        if (victim < 0) victim = raster_source_victim(d, d->tiles[selected].z);
        if (victim < 0) return false; /* do not thrash visible target tiles */
        raster_drop(d, &d->images[victim]);
    }
    int image = -1;
    for (int i = 0; i < SLOTS; i++) if (!d->images[i].raster) { image = i; break; }
    if (image < 0) {
        image = map_demo_raster_victim(d->images, SLOTS, d->frame - 1);
        if (image < 0) image = raster_source_victim(d, d->tiles[selected].z);
        if (image < 0) return false;
        raster_drop(d, &d->images[image]);
    }
    if (d->raster_capacity < bytes) {
        void *next = realloc(d->raster_pixels, bytes);
        if (!next) { d->raster_failed = true; return false; }
        d->raster_pixels = static_cast<unsigned char *>(next); d->raster_capacity = bytes;
    }
    slot *s = &d->tiles[selected];
    cui_begin_frame(d->raster_ui, 1.f / 60);
    cui_map_tile_draw_base_styled(d->raster_ui, s->tile, cui_rect{0, 0, 256, 256}, s->z, &palette);
    cui_end_frame(d->raster_ui);
    cui_sw_render(d->raster_sw, cui_get_draw_data(d->raster_ui), d->raster_pixels,
                  pixels, pixels, pixels * 4, CUI_SW_RGBA);
    cui_texture texture = cui_image_create(d->ui, pixels, pixels, d->raster_pixels);
    if (!texture) d->raster_failed = true;
    if (texture) {
        d->images[image] = map_demo_raster_slot{s->z, s->x, s->y, texture, d->frame};
        d->raster_bytes += bytes;
    }
    return texture != 0;
}
static void cache_drop(demo *d, slot *s) {
    if (s->state == 3 && d->failures > 0) d->failures--;
    if (s->tile) {
        size_t bytes = cui_mvt_tile_memory_usage(s->tile);
        d->cache_bytes -= bytes;
        cui_mvt_tile_destroy(s->tile);
    }
    *s = slot{};
}
static bool cache_make_room(demo *d, size_t bytes, int target_zoom) {
    if (bytes > CACHE_BUDGET) return false;
    /* pump runs after cui_app_frame has presented the preceding draw data and
     * before building the next map: last frame's lookup pointers are no longer
     * in use. Pinning used == d->frame here made a near-full cache reject
     * completed downloads as permanent errors during pan/zoom. */
    while (d->cache_bytes > CACHE_BUDGET - bytes) {
        int victim = map_demo_decoded_victim(d->tiles, SLOTS, target_zoom);
        if (victim < 0) return false;
        cache_drop(d, &d->tiles[victim]);
    }
    return true;
}
static void pump(demo *d) {
    result results[RESULT_QUEUE]; int count = 0;
    EnterCriticalSection(&d->lock);
    while (d->rcount) {
        results[count++] = d->results[d->rhead];
        d->rhead = (d->rhead + 1) % RESULT_QUEUE; d->rcount--;
    }
    LeaveCriticalSection(&d->lock);
    for (int i = 0; i < count; i++) {
        result *r = &results[i];
#ifdef CUI_MAP_HOST_LIBRARY
        if (r->network_epoch != d->network_epoch) {
            free(r->json); cui_mvt_tile_destroy(r->tile);
            continue;
        }
#endif
        if (r->z < 0) {
            d->refreshing = false;
            char previous[sizeof d->template_path];
            memcpy(previous, d->template_path, sizeof previous);
            if (r->ok && template_from_json(d, r->json)) {
                if (previous[0] && strcmp(previous, d->template_path)) {
                    /* Old tiles and images must never be shown under a new
                     * TileJSON source. Running jobs retain old tokens and the
                     * queued old-version work becomes obsolete. */
                    for (int k = 0; k < SLOTS; k++) cache_drop(d, &d->tiles[k]);
                    raster_invalidate(d);
                    EnterCriticalSection(&d->lock);
                    d->jhead = d->jtail; d->jcount = 0;
                    ResetEvent(d->available);
                    LeaveCriticalSection(&d->lock);
                    d->failures = 0;
                }
                d->tilejson = 1; d->offline = r->offline;
                if (r->offline) d->last_status = r->status;
            } else if (d->tilejson != 1) {
                d->tilejson = -1; d->last_status = r->status;
            } else d->last_status = r->status; /* failed refresh: keep offline map */
            free(r->json);
        } else {
            slot *s = NULL;
            for (int k = 0; k < SLOTS; k++) if (d->tiles[k].state == 1 && d->tiles[k].z == r->z && d->tiles[k].x == r->x && d->tiles[k].y == r->y) { s = &d->tiles[k]; break; }
            if (s && s->token == r->token && s->z == r->z && s->x == r->x && s->y == r->y) {
                size_t bytes = cui_mvt_tile_memory_usage(r->tile);
                if (r->ok && cache_make_room(d, bytes, r->z)) {
                    s->tile = r->tile; d->cache_bytes += bytes; s->state = 2;
                    if (r->cached_tile) d->disk_hits++; else d->network_hits++;
                } else {
                    if (r->ok) d->last_status = bytes > CACHE_BUDGET ? 508 : 507;
                    else d->last_status = r->status;
                    cui_mvt_tile_destroy(r->tile); d->failures++;
                    map_demo_note_failure(*s, d->last_status, r->retry_after_ms, GetTickCount());
                }
            } else cui_mvt_tile_destroy(r->tile);
        }
    }
}
static slot *find(demo *d, int z, int x, int y) {
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &d->tiles[i];
        if (s->state && s->z == z && s->x == x && s->y == y) return s;
    }
    return NULL;
}
static const cui_mvt_tile *tile_lookup(void *user, int z, int x, int y) {
    demo *d = static_cast<demo *>(user); slot *s = find(d, z, x, y);
    if (s) { s->used = d->frame; if (s->tile) { d->hits++; return s->tile; } }
    d->misses++;
    return NULL;
}
static void tile_request(void *user, int z, int x, int y) {
    demo *d = static_cast<demo *>(user);
    slot *existing = find(d, z, x, y);
    bool retry = existing && map_demo_retry_due(*existing, GetTickCount());
    if (d->tilejson != 1 || (existing && !retry)) return;
    /* When decoded memory is saturated, an image-only tile can still be
     * displayed. Do not fetch it again on every drag frame just for labels;
     * reserve requests for tiles with no usable image. */
    if (!existing && d->cache_bytes >= CACHE_BUDGET * 9 / 10 &&
        raster_find(d, z, x, y)) return;
    int index = existing ? (int)(existing - d->tiles) : -1; unsigned oldest = ~0u;
    if (index < 0) for (int i = 0; i < SLOTS; i++) {
        slot *s = &d->tiles[i];
        if (!s->state) { index = i; break; }
        if (s->state != 1 && s->used != d->frame && s->used < oldest) { oldest = s->used; index = i; }
    }
    if (index < 0) return;
    slot *s = &d->tiles[index];
    char path[240]; const char *token = strstr(d->template_path, "{z}/{x}/{y}");
    if (!token) return;
    int prefix = (int)(token - d->template_path);
    int n = snprintf(path, sizeof path, "%.*s%d/%d/%d%s", prefix, d->template_path, z, x, y, token + 11);
    if (n < 0 || n >= (int)sizeof path) return;
    /* Enqueue before replacing the slot: if saturated, try on the next frame. */
    EnterCriticalSection(&d->lock);
    bool space = d->jcount < QUEUE && !d->stopping;
    LeaveCriticalSection(&d->lock);
    if (!space) return;
    unsigned retry_count = retry ? s->retry_count + (s->status != 507) : 0;
    cache_drop(d, s);
    d->next_token++;
    *s = slot{z, x, y, 1, d->frame, NULL, d->next_token};
    s->retry_count = retry_count;
    job j = {z, x, y, "", d->next_token}; memcpy(j.path, path, (size_t)n + 1);
    memcpy(j.tile_template, d->template_path, strlen(d->template_path) + 1);
    if (!queue_job(d, j)) { *s = slot{}; return; }
}
static void stop(demo *d, bool ui_alive = true) {
    EnterCriticalSection(&d->lock); d->stopping = true; LeaveCriticalSection(&d->lock);
    SetEvent(d->stop_event);
    SetEvent(d->available);
    WaitForMultipleObjects(WORKERS, d->threads, TRUE, INFINITE);
    for (int i = 0; i < WORKERS; i++) CloseHandle(d->threads[i]);
    CloseHandle(d->available); CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock);
    if (ui_alive) raster_invalidate(d);
    for (int i = 0; i < SLOTS; i++) cui_mvt_tile_destroy(d->tiles[i].tile);
    free(d->raster_pixels);
    cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw);
    while (d->rcount) {
        result *r = &d->results[d->rhead]; free(r->json); cui_mvt_tile_destroy(r->tile);
        d->rhead = (d->rhead + 1) % RESULT_QUEUE; d->rcount--;
    }
    delete d->disk;
}
#ifndef CUI_MAP_HOST_LIBRARY
static void map_profile_report(FILE *out, const demo *d, unsigned frames,
                               const double *sum, const double *peak,
                               unsigned long long hits, unsigned long long misses,
                               unsigned long long raster_hits) {
    if (!out || !frames) return;
    fprintf(out, "map profile: frames=%u z%d pump=%.2f/%.2fms tile_bake=%.2f/%.2fms ui_build=%.2f/%.2fms tile_lookups=%llu/%llu raster_hits=%llu raster=%.1fMiB (avg/max; hits/misses; before paint)\n",
            frames, (int)d->view.zoom,
            sum[0] / frames, peak[0], sum[1] / frames, peak[1],
            sum[2] / frames, peak[2], hits, misses, raster_hits,
            (double)d->raster_bytes / (1024 * 1024));
    fflush(out);
}
int main(void) {
    demo *d = static_cast<demo *>(calloc(1, sizeof *d));
    if (!d) return 1;
    d->view = cui_map_view{52.52, 13.405, 8}; d->selected = -1;
    if (getenv("CUI_MAP_DEMO_BEIJING")) d->view = cui_map_view{39.9, 116.4, 7};
    InitializeCriticalSection(&d->lock);
    d->available = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    cui_app_desc desc = {};
    desc.title = "CUI native vector map (OpenFreeMap)";
    desc.width = getenv("CUI_MAP_DEMO_SMALL") ? 460 : 1000;
    desc.height = getenv("CUI_MAP_DEMO_SMALL") ? 360 : 680;
    desc.min_width = 450; desc.min_height = 360;
    desc.theme = CUI_THEME_LIGHT;
    desc.follow_system_theme = true;
    desc.accent = CUI_ACCENT_BLUE;
    /* Dense vector geometry can make the default GL driver's staging buffers
     * dwarf the decoded-tile cache. Keep this example's memory predictable;
     * CUI's renderer default and map widget are unchanged. */
    desc.renderer = getenv("CUI_MAP_DEMO_GL") ? CUI_RENDERER_AUTO : CUI_RENDERER_SOFTWARE;
    desc.continuous = getenv("CUI_SCREENSHOT") != NULL || getenv("CUI_MAP_DEMO_BENCH") != NULL;
    cui_app *app = d->available && d->stop_event ? cui_app_create(&desc) : NULL;
    if (!app) { if (d->available) CloseHandle(d->available); if (d->stop_event) CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock); free(d); return 1; }
    d->ui = cui_app_ui(app);
    install_local_cjk_fallback(d->ui);
    cui_ctx_desc raster_desc = {};
    raster_desc.theme = cui_get_theme_mode(d->ui); raster_desc.no_default_fonts = true;
    d->raster_ui = cui_create(&raster_desc);
    d->raster_sw = cui_sw_create();
    if (!d->raster_ui || !d->raster_sw) {
        cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw);
        d->raster_ui = NULL; d->raster_sw = NULL;
    }
    d->window = (HWND)cui_app_native_window(app);
    d->disk = new (std::nothrow) map_disk_cache(); /* unavailable disk degrades to network */
    int started = 0;
    for (; started < WORKERS; started++) {
        d->threads[started] = CreateThread(NULL, 0, worker, d, 0, NULL);
        if (!d->threads[started]) break;
    }
    if (started != WORKERS) {
        EnterCriticalSection(&d->lock); d->stopping = true; LeaveCriticalSection(&d->lock);
        SetEvent(d->stop_event);
        SetEvent(d->available);
        if (started) WaitForMultipleObjects(started, d->threads, TRUE, INFINITE);
        for (int i = 0; i < started; i++) CloseHandle(d->threads[i]);
        CloseHandle(d->available); CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock); delete d->disk; cui_app_destroy(app); free(d); return 1;
    }
    job first = {-1, 0, 0, "/planet", 1};
    if (getenv("CUI_MAP_DEMO_OFFLINE")) d->tilejson = -1; /* deterministic failure-state smoke test */
    else queue_job(d, first);
    const cui_map_marker markers[] = {{0, 52.52, 13.405, "Berlin"}, {1, 48.8566, 2.3522, "Paris"}};
    LARGE_INTEGER perf_freq = {}, perf_begin = {}, perf_end = {};
    QueryPerformanceFrequency(&perf_freq);
    double drag_ms = 0, drag_peak_ms = 0;
    unsigned long long drag_vertices = 0;
    int drag_samples = 0;
    bool profile = getenv("CUI_APP_PROFILE") && !strcmp(getenv("CUI_APP_PROFILE"), "1") && cui_app_is_software(app);
    FILE *profile_log = NULL;
    if (profile) {
        const char *path = getenv("CUI_APP_PROFILE_FILE");
        if (!path || !*path) path = "map-profile.log";
        profile_log = fopen(path, "a");
        profile = profile_log != NULL;
        if (profile) {
            fprintf(profile_log, "map profile: started (output=%s)\n", path);
            fflush(profile_log);
        }
    }
    double profile_sum[3] = {}, profile_max[3] = {};
    unsigned profile_frames = 0;
    unsigned long long profile_hits = 0, profile_misses = 0, profile_raster_hits = 0;
    for (;;) {
        if (getenv("CUI_MAP_DEMO_BENCH")) QueryPerformanceCounter(&perf_begin);
        if (!cui_app_frame(app)) break;
        if (getenv("CUI_MAP_DEMO_BENCH")) {
            QueryPerformanceCounter(&perf_end);
            if (d->frame >= 650 && d->frame < 750) {
                double ms = (double)(perf_end.QuadPart - perf_begin.QuadPart) * 1000.0 / perf_freq.QuadPart;
                drag_ms += ms; if (ms > drag_peak_ms) drag_peak_ms = ms;
                drag_vertices += (unsigned)cui_get_draw_data(d->ui)->vertex_count;
                drag_samples++;
            } else if (d->frame == 750 && drag_samples) {
                fprintf(stderr, "map drag probe: frames=%d app_frame_avg=%.2fms max=%.2fms vertices_avg=%llu raster=%.1fMiB\n",
                        drag_samples, drag_ms / drag_samples, drag_peak_ms,
                        drag_vertices / (unsigned long long)drag_samples,
                        (double)d->raster_bytes / (1024 * 1024));
            }
        }
        cui_ctx *ui = cui_app_ui(app);
        if (getenv("CUI_MAP_DEMO_ZOOM_PROBE") || getenv("CUI_MAP_DEMO_ZOOM_OUT_PROBE")) {
            if (d->frame == 399) cui_input_mouse_move(ui, 500, 300);
            if (d->frame == 400) cui_input_scroll(ui, 0,
                getenv("CUI_MAP_DEMO_ZOOM_OUT_PROBE") ? -1 : 1);
        }
        if (getenv("CUI_MAP_DEMO_BENCH") && d->frame >= 649 && d->frame < 750) {
            cui_input_mouse_move(ui, 500 + ((int)(d->frame - 649) % 50), 300);
            if (d->frame == 649) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
            if (d->frame == 749) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        }
        LARGE_INTEGER stage_start = {}, stage_pump = {}, stage_bake = {}, stage_end = {};
        if (profile) QueryPerformanceCounter(&stage_start);
        pump(d); d->frame++;
        if (profile) QueryPerformanceCounter(&stage_pump);
        bool baked = !getenv("CUI_MAP_DEMO_DISABLE_RASTER") && raster_prepare(d);
        if (profile) QueryPerformanceCounter(&stage_bake);
        cui_box root = {}, header = {}, footer = {};
        root.width = CUI_FILL; root.height = CUI_FILL;
        header.pad = 12; header.gap = 16; header.width = CUI_FILL;
        footer.pad = 10; footer.gap = 4; footer.width = CUI_FILL;
        cui_column_begin(ui, &root);
        cui_row_begin(ui, &header);
        cui_heading(ui, "Map", 3);
        if (d->tilejson == 0) cui_secondary(ui, "Connecting to OpenFreeMap...");
        else if (d->tilejson < 0) {
            cui_secondary(ui, (d->last_status == 429 || d->last_status == 503) ? "OpenFreeMap is throttling requests." :
                              d->last_status == 200 ? "Invalid TileJSON from tile service." :
                              "Tile service unavailable.");
            if (cui_button_ex(ui, "Retry", nullptr)) { d->tilejson = 0; queue_job(d, first); }
        } else if (d->offline) {
            cui_secondary(ui, d->refreshing ? "Checking online TileJSON..." :
                                       "Offline · showing locally cached tiles");
            if (!d->refreshing && cui_button_ex(ui, "Retry online", nullptr))
                d->refreshing = queue_job(d, first);
        } else if (d->failures) {
            cui_textf(ui, "%d tiles unavailable%s", d->failures,
                      (d->last_status == 429 || d->last_status == 503) ? " (throttled)" :
                      d->last_status == 507 ? " (cache busy; retrying)" :
                      d->last_status == 508 ? " (tile exceeds cache budget)" :
                      d->last_status == map_host_tile_body_too_large ? " (tile exceeds 2 MiB input limit)" :
                      d->last_status == 200 ? " (MVT decode failed)" :
                      d->last_status >= 400 ? " (HTTP error)" : " (network error)");
            if (cui_button_ex(ui, "Retry", nullptr)) {
                for (int i = 0; i < SLOTS; i++) if (d->tiles[i].state == 3) d->tiles[i].state = 0;
                d->failures = 0;
            }
        } else if (d->pending) {
            cui_textf(ui, "Loading %d tiles · %d ready", d->pending, d->loaded);
        } else cui_secondary(ui, "Drag to pan  ·  Wheel to zoom  ·  Arrow keys to move");
        cui_button_opts theme_toggle = {};
        theme_toggle.tooltip = "Switch map between light and dark";
        if (cui_icon_button_ex(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_ICON_SUN : CUI_ICON_MOON,
                               &theme_toggle)) {
            cui_app_follow_system_theme(app, false);
            cui_set_theme(ui, cui_get_theme_mode(ui) == CUI_THEME_DARK ? CUI_THEME_LIGHT : CUI_THEME_DARK);
            raster_invalidate(d);
            d->raster_palette_valid = false;
            cui_request_redraw(ui);
        }
        cui_box_end(ui);
        cui_map_opts options = {};
        cui_map_palette palette = cui_map_palette_default(cui_get_theme_mode(ui));
        options.palette = &palette;
        options.lookup = tile_lookup;
        if (!getenv("CUI_MAP_DEMO_DISABLE_RASTER")) options.raster_lookup = raster_lookup;
        float image_scale = cui_dpi_scale(ui);
        int image_pixels = image_scale > 0 && image_scale <= 4 ? (int)(256 * image_scale + 0.5f) : 0;
        options.defer_vector_base = options.raster_lookup && d->raster_ui && d->raster_sw &&
                                    !d->raster_failed && image_scale > 0 && image_scale <= 4 &&
                                    (size_t)image_pixels * image_pixels * 4 <= RASTER_BUDGET;
        options.request = tile_request; options.tile_user = d;
        options.markers = markers; options.marker_count = 2;
        options.min_zoom = d->min_zoom; options.max_zoom = d->max_zoom ? d->max_zoom : 14;
        cui_map_result hit = cui_map_ex(ui, "online-map", &d->view, &options);
        if (baked) for (int i = 0; i < SLOTS; i++) {
            slot *s = &d->tiles[i];
            if (s->state == 2 && s->used == d->frame && !raster_find(d, s->z, s->x, s->y) &&
                (d->raster_bytes <= RASTER_BUDGET - d->raster_tile_bytes ||
                 map_demo_raster_victim(d->images, SLOTS, d->frame - 1) >= 0 ||
                 raster_source_victim(d, s->z) >= 0)) {
                cui_request_redraw(ui); break;
            }
        }
        EnterCriticalSection(&d->lock);
        map_demo_cancel_stale(d->tiles, SLOTS, d->jobs, QUEUE, d->jhead, d->jtail, d->jcount, d->frame);
        if (!d->jcount) ResetEvent(d->available);
        LeaveCriticalSection(&d->lock);
        d->pending = d->loaded = 0;
        for (int i = 0; i < SLOTS; i++) if (d->tiles[i].used == d->frame) {
            if (d->tiles[i].state == 1) d->pending++;
            if (d->tiles[i].state == 2) d->loaded++;
        }
        if (hit.clicked_marker >= 0) d->selected = hit.clicked_marker;
        cui_column_begin(ui, &footer);
        cui_text_ex(ui, "OpenFreeMap © OpenMapTiles Data from OpenStreetMap", nullptr);
        cui_textf(ui, "z%d · Cache %.1f/192 MiB · Raster %.1f/96 MiB · D%llu/N%llu",
                  (int)d->view.zoom, (double)d->cache_bytes / (1024 * 1024),
                  (double)d->raster_bytes / (1024 * 1024), d->disk_hits, d->network_hits);
        bool shown_failure = false;
        for (int i = 0; i < SLOTS; i++) {
            slot *s = &d->tiles[i];
            if (s->state != 3 || s->used != d->frame) continue;
            if (!shown_failure) {
                if (s->status == 200) cui_textf(ui, "Tile z%d/%d/%d: MVT decode failed", s->z, s->x, s->y);
                else if (s->status == 507) cui_textf(ui, "Tile z%d/%d/%d: cache busy; retrying", s->z, s->x, s->y);
                else if (s->status == 508) cui_textf(ui, "Tile z%d/%d/%d: exceeds cache budget", s->z, s->x, s->y);
                else if (s->status == map_host_tile_body_too_large) cui_textf(ui, "Tile z%d/%d/%d: exceeds 2 MiB input limit", s->z, s->x, s->y);
                else if (s->status) cui_textf(ui, "Tile z%d/%d/%d: HTTP %u", s->z, s->x, s->y, s->status);
                else cui_textf(ui, "Tile z%d/%d/%d: network error", s->z, s->x, s->y);
                shown_failure = true;
            }
            if (s->next_retry_tick && (s->retry_count < 2 || s->status == 507)) {
                int32_t wait_ms = (int32_t)(s->next_retry_tick - GetTickCount());
                if (wait_ms > 0) cui_request_wakeup(ui, (float)wait_ms / 1000.f);
            }
        }
        if (d->selected >= 0) cui_textf(ui, "Marker: %s", markers[d->selected].label);
        cui_box_end(ui);
        cui_box_end(ui);
        if (profile) {
            QueryPerformanceCounter(&stage_end);
            double ms[3] = {
                (double)(stage_pump.QuadPart - stage_start.QuadPart) * 1000.0 / perf_freq.QuadPart,
                (double)(stage_bake.QuadPart - stage_pump.QuadPart) * 1000.0 / perf_freq.QuadPart,
                (double)(stage_end.QuadPart - stage_bake.QuadPart) * 1000.0 / perf_freq.QuadPart
            };
            for (int i = 0; i < 3; i++) {
                profile_sum[i] += ms[i];
                if (ms[i] > profile_max[i]) profile_max[i] = ms[i];
            }
            if (++profile_frames == 60) {
                map_profile_report(profile_log, d, profile_frames, profile_sum, profile_max,
                                   d->hits - profile_hits, d->misses - profile_misses,
                                   d->raster_hits - profile_raster_hits);
                profile_frames = 0;
                memset(profile_sum, 0, sizeof profile_sum);
                memset(profile_max, 0, sizeof profile_max);
                profile_hits = d->hits; profile_misses = d->misses; profile_raster_hits = d->raster_hits;
            }
        }
    }
    map_profile_report(profile_log, d, profile_frames, profile_sum, profile_max,
                       d->hits - profile_hits, d->misses - profile_misses,
                       d->raster_hits - profile_raster_hits);
    if (profile_log) fclose(profile_log);
    stop(d); cui_app_destroy(app); free(d);
    return 0;
}
#else
static bool valid_connection(const map_host_connection &connection) {
    if (connection.mode != map_host_proxy_mode::System &&
        connection.mode != map_host_proxy_mode::Direct &&
        connection.mode != map_host_proxy_mode::Manual) return false;
    if (connection.mode != map_host_proxy_mode::Manual) return true;
    if (!connection.manual_proxy) return false;
    size_t size = 0;
    while (size < 260 && connection.manual_proxy[size]) ++size;
    return size > 0 && size < 260;
}
static void copy_connection(demo *d, const map_host_connection &connection) {
    d->network_mode = connection.mode;
    d->manual_proxy[0] = 0;
    if (connection.mode == map_host_proxy_mode::Manual)
        memcpy(d->manual_proxy, connection.manual_proxy,
               (wcslen(connection.manual_proxy) + 1) * sizeof(wchar_t));
}
map_host_win32 *map_host_create(cui_ctx *ui, void *native_window,
                                const map_host_connection &connection) {
    if (!valid_connection(connection)) return NULL;
    demo *d = static_cast<demo *>(calloc(1, sizeof *d));
    if (!d) return NULL;
    InitializeCriticalSection(&d->lock);
    d->available = CreateEventW(NULL, TRUE, FALSE, NULL);
    d->stop_event = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!d->available || !d->stop_event) {
        if (d->available) CloseHandle(d->available);
        if (d->stop_event) CloseHandle(d->stop_event);
        DeleteCriticalSection(&d->lock); free(d); return NULL;
    }
    d->ui = ui;
    d->window = (HWND)native_window;
    copy_connection(d, connection);
    d->network_epoch = 1;
    cui_ctx_desc raster_desc = {};
    raster_desc.theme = cui_get_theme_mode(ui); raster_desc.no_default_fonts = true;
    d->raster_ui = cui_create(&raster_desc);
    d->raster_sw = cui_sw_create();
    if (!d->raster_ui || !d->raster_sw) {
        cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw);
        d->raster_ui = NULL; d->raster_sw = NULL;
    }
    d->disk = new (std::nothrow) map_disk_cache();
    /* A previously validated, size-bounded TileJSON lets cached tiles draw
     * immediately. Refresh the metadata in the background; pump() replaces
     * the source and invalidates old tiles if the provider version changed. */
    if (d->disk) {
        std::string saved;
        if (d->disk->read_tilejson(saved) && template_from_json(d, saved)) {
            d->tilejson = 1;
            d->offline = true;
        }
    }
    int started = 0;
    for (; started < WORKERS; ++started) {
        d->threads[started] = CreateThread(NULL, 0, worker, d, 0, NULL);
        if (!d->threads[started]) break;
    }
    if (started != WORKERS) {
        EnterCriticalSection(&d->lock); d->stopping = true; LeaveCriticalSection(&d->lock);
        SetEvent(d->stop_event); SetEvent(d->available);
        if (started) WaitForMultipleObjects(started, d->threads, TRUE, INFINITE);
        for (int i = 0; i < started; ++i) CloseHandle(d->threads[i]);
        CloseHandle(d->available); CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock);
        cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw); delete d->disk; free(d);
        return NULL;
    }
    job first = {-1, 0, 0, "/planet", 1};
    if (!queue_job(d, first)) { stop(d); free(d); return NULL; }
    d->refreshing = true;
    return reinterpret_cast<map_host_win32 *>(d);
}
void map_host_begin_frame(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    EnterCriticalSection(&d->lock);
    bool resumed = d->paused;
    d->paused = false;
    LeaveCriticalSection(&d->lock);
    if ((resumed || d->tilejson == 0) && !d->refreshing) map_host_retry(host);
    pump(d); ++d->frame;
    raster_prepare(d);
}
void map_host_options(map_host_win32 *host, cui_map_opts *options) {
    if (!host || !options) return;
    demo *d = reinterpret_cast<demo *>(host);
    options->lookup = tile_lookup;
    options->request = tile_request;
    options->raster_lookup = d->raster_ui && d->raster_sw && !d->raster_failed ? raster_lookup : NULL;
    options->defer_vector_base = options->raster_lookup != NULL;
    options->tile_user = d;
    options->min_zoom = d->min_zoom;
    options->max_zoom = d->max_zoom ? d->max_zoom : 14;
}
void map_host_end_frame(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    EnterCriticalSection(&d->lock);
    map_demo_cancel_stale(d->tiles, SLOTS, d->jobs, QUEUE, d->jhead, d->jtail, d->jcount, d->frame);
    if (!d->jcount) ResetEvent(d->available);
    LeaveCriticalSection(&d->lock);
    for (int i = 0; i < SLOTS; ++i) {
        slot *s = &d->tiles[i];
        if (s->state == 2 && s->tile && s->used == d->frame && d->raster_ui && d->raster_sw &&
            !d->raster_failed && d->raster_tile_bytes && !raster_find(d, s->z, s->x, s->y) &&
            (d->raster_bytes <= RASTER_BUDGET - d->raster_tile_bytes ||
             map_demo_raster_victim(d->images, SLOTS, d->frame - 1) >= 0 ||
             raster_source_victim(d, s->z) >= 0)) { cui_request_redraw(d->ui); break; }
        if (s->state == 3 && s->used == d->frame && s->next_retry_tick) {
            int32_t wait_ms = (int32_t)(s->next_retry_tick - GetTickCount());
            if (wait_ms > 0) cui_request_wakeup(d->ui, (float)wait_ms / 1000.f);
        }
    }
}
void map_host_pause(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    EnterCriticalSection(&d->lock);
    d->paused = true;
    ++d->network_epoch; /* in-flight metadata has no slot token; reject it on resume */
    d->jhead = d->jtail; d->jcount = 0;
    ResetEvent(d->available);
    while (d->rcount) {
        result *r = &d->results[d->rhead];
        free(r->json); cui_mvt_tile_destroy(r->tile);
        d->rhead = (d->rhead + 1) % RESULT_QUEUE; --d->rcount;
    }
    d->refreshing = false; /* the queued refresh may have been canceled */
    LeaveCriticalSection(&d->lock);
    for (int i = 0; i < SLOTS; ++i)
        if (d->tiles[i].state == 1) cache_drop(d, &d->tiles[i]);
}
void map_host_retry(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    if (d->refreshing) return;
    job refresh = {-1, 0, 0, "/planet", ++d->next_token};
    if (queue_job(d, refresh)) { d->refreshing = true; if (d->tilejson != 1) d->tilejson = 0; }
}
bool map_host_visible_failure(map_host_win32 *host, map_host_tile_failure *out) {
    if (!host || !out) return false;
    demo *d = reinterpret_cast<demo *>(host);
    for (int i = 0; i < SLOTS; ++i) {
        const slot &s = d->tiles[i];
        if (s.state == 3 && s.used == d->frame) {
            *out = map_host_tile_failure{s.z, s.x, s.y, s.status};
            return true;
        }
    }
    return false;
}
void map_host_retry_tiles(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    for (int i = 0; i < SLOTS; ++i)
        if (d->tiles[i].state == 3) cache_drop(d, &d->tiles[i]);
    cui_request_redraw(d->ui);
}
bool map_host_set_connection(map_host_win32 *host, const map_host_connection &connection) {
    if (!host || !valid_connection(connection)) return false;
    demo *d = reinterpret_cast<demo *>(host);
    EnterCriticalSection(&d->lock);
    ++d->network_epoch;
    copy_connection(d, connection);
    d->jhead = d->jtail; d->jcount = 0;
    ResetEvent(d->available);
    d->refreshing = false;
    bool paused = d->paused;
    LeaveCriticalSection(&d->lock);
    /* Only pending and failed slots are dropped. Ready decoded tiles and
     * raster textures can still be referenced by the current UI frame. */
    for (int i = 0; i < SLOTS; ++i)
        if (d->tiles[i].state == 1 || d->tiles[i].state == 3) cache_drop(d, &d->tiles[i]);
    d->last_status = 0;
    d->tilejson = d->template_path[0] ? 1 : 0;
    if (!paused) map_host_retry(host);
    cui_request_redraw(d->ui);
    return true;
}
int map_host_status(const map_host_win32 *host) {
    return host ? reinterpret_cast<const demo *>(host)->tilejson : -1;
}
unsigned map_host_last_http(const map_host_win32 *host) {
    return host ? reinterpret_cast<const demo *>(host)->last_status : 0;
}
void map_host_destroy_after_app(map_host_win32 *host) {
    if (!host) return;
    demo *d = reinterpret_cast<demo *>(host);
    stop(d, false); /* UI textures belong to the already destroyed app. */
    free(d);
}
#endif
