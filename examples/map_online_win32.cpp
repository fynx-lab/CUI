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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SLOTS 192
#define QUEUE 64
#define RESULT_QUEUE 8
#define CACHE_BUDGET (96u * 1024u * 1024u)
#define RASTER_BUDGET (48u * 1024u * 1024u)
#define WORKERS 4
#define BODY_CAP (2 * 1024 * 1024)
typedef map_demo_slot slot;
typedef map_demo_job job;
typedef struct { int z, x, y, ok; DWORD status; unsigned retry_after_ms; uint64_t token; char *json; cui_mvt_tile *tile; } result;
typedef struct {
    CRITICAL_SECTION lock;
    HANDLE available, stop_event, threads[WORKERS];
    HWND window; bool stopping;
    job jobs[QUEUE]; int jhead, jtail, jcount;
    result results[RESULT_QUEUE]; int rhead, rtail, rcount;
    slot tiles[SLOTS]; unsigned frame; size_t cache_bytes;
    cui_ctx *ui, *raster_ui; cui_sw_renderer *raster_sw;
    unsigned char *raster_pixels; size_t raster_capacity, raster_bytes, raster_tile_bytes;
    float raster_scale;
    char template_path[240]; int min_zoom, max_zoom, tilejson; int failures, selected;
    int pending, loaded; unsigned long long hits, misses; DWORD last_status; uint64_t next_token;
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
static int fetch(const char *path, char **out, int cap, DWORD *status, unsigned *retry_after_ms) {
    int n = -1; *out = NULL; *status = 0; *retry_after_ms = 0;
    wchar_t wide[256];
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, path, -1, wide, 256)) return -1;
    HINTERNET session = WinHttpOpen(L"CUI-Map-Demo/0.1 (+https://github.com/fynx-lab/CUI)",
                                    WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
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
        if (n > cap) { n = -1; break; }
    }
    if (n > 0) { bytes[n] = 0; *out = bytes; }
    else { free(bytes); n = -1; }
done:
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
        if (!d->jcount) ResetEvent(d->available);
        LeaveCriticalSection(&d->lock);
        char *bytes = NULL;
        DWORD status = 0; unsigned retry_after_ms = 0;
        int n = -1;
        for (int attempt = 1; attempt <= 3; attempt++) {
            /* Deterministic transport-failure smoke test; no tile HTTP in this mode. */
            const char *fault = j.z >= 0 ? getenv("CUI_MAP_DEMO_TILE_FAULT") : NULL;
            if (fault && (!strcmp(fault, "404") || !strcmp(fault, "503") || !strcmp(fault, "0"))) {
                n = -1; status = (DWORD)atoi(fault); retry_after_ms = 0;
            } else n = fetch(j.path, &bytes, j.z < 0 ? 65536 : BODY_CAP, &status, &retry_after_ms);
            if (n > 0 || status == 200 || (status && status != 429 && status != 503 && status < 500) || attempt == 3) break;
            unsigned delay = map_demo_retry_delay_ms(attempt, retry_after_ms,
                                                      GetTickCount() ^ (unsigned)j.token);
            if (WaitForSingleObject(d->stop_event, delay) != WAIT_TIMEOUT) break;
        }
        result r = {j.z, j.x, j.y, n > 0, status, retry_after_ms, j.token, NULL, NULL};
        if (j.z < 0) r.json = bytes;
        else { if (n > 0) r.tile = cui_mvt_tile_decode(bytes, (size_t)n); r.ok = r.tile != NULL; free(bytes); }
        /* A slow UI frame can finish more than 68 jobs before it drains the
         * result queue. Never drop a result and strand its slot as pending. */
        for (;;) {
            EnterCriticalSection(&d->lock);
            if (d->stopping) {
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
static bool template_from_json(demo *d, const char *json) {
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
static void raster_drop(demo *d, slot *s) {
    if (!s->raster) return;
    cui_image_destroy(d->ui, s->raster);
    d->raster_bytes -= d->raster_tile_bytes;
    s->raster = 0; s->raster_used = 0;
}
/* Only called between frames: textures returned to the map remain alive until
 * cui_end_frame, even when cache pressure or a tile replacement follows. */
static void raster_invalidate(demo *d) {
    for (int i = 0; i < SLOTS; i++) raster_drop(d, &d->tiles[i]);
}
static cui_texture raster_lookup(void *user, int z, int x, int y) {
    demo *d = static_cast<demo *>(user);
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &d->tiles[i];
        if (s->raster && s->state == 2 && s->z == z && s->x == x && s->y == y) {
            s->raster_used = d->frame; return s->raster;
        }
    }
    return 0;
}
static bool raster_prepare(demo *d) {
    if (!d->raster_ui || !d->raster_sw) return false;
    float scale = cui_dpi_scale(d->ui);
    if (scale <= 0 || scale > 4) return false;
    int pixels = (int)(256 * scale + 0.5f);
    size_t bytes = (size_t)pixels * pixels * 4;
    if (bytes > RASTER_BUDGET) return false;
    if (d->raster_scale != scale) {
        raster_invalidate(d); d->raster_scale = scale;
        d->raster_tile_bytes = bytes;
        cui_set_viewport(d->raster_ui, 256, 256, scale);
    }
    int selected = -1;
    for (int i = 0; i < SLOTS; i++) {
        slot *s = &d->tiles[i];
        if (s->state == 2 && s->tile && !s->raster && s->used == d->frame - 1) {
            selected = i; break;
        }
    }
    if (selected < 0) return false;
    while (d->raster_bytes > RASTER_BUDGET - bytes) {
        int victim = map_demo_raster_victim(d->tiles, SLOTS, d->frame - 1);
        if (victim < 0) return false; /* keep visible tiles rather than thrash */
        raster_drop(d, &d->tiles[victim]);
    }
    if (d->raster_capacity < bytes) {
        void *next = realloc(d->raster_pixels, bytes);
        if (!next) return false;
        d->raster_pixels = static_cast<unsigned char *>(next); d->raster_capacity = bytes;
    }
    slot *s = &d->tiles[selected];
    cui_begin_frame(d->raster_ui, 1.f / 60);
    cui_map_tile_draw_base(d->raster_ui, s->tile, cui_rect{0, 0, 256, 256}, s->z);
    cui_end_frame(d->raster_ui);
    cui_sw_render(d->raster_sw, cui_get_draw_data(d->raster_ui), d->raster_pixels,
                  pixels, pixels, pixels * 4, CUI_SW_RGBA);
    s->raster = cui_image_create(d->ui, pixels, pixels, d->raster_pixels);
    if (s->raster) { s->raster_used = d->frame; d->raster_bytes += bytes; }
    return s->raster != 0;
}
static void cache_drop(demo *d, slot *s) {
    raster_drop(d, s);
    if (s->state == 3 && d->failures > 0) d->failures--;
    if (s->tile) {
        size_t bytes = cui_mvt_tile_memory_usage(s->tile);
        d->cache_bytes -= bytes;
        cui_mvt_tile_destroy(s->tile);
    }
    *s = slot{};
}
static bool cache_make_room(demo *d, size_t bytes) {
    if (bytes > CACHE_BUDGET) return false;
    while (d->cache_bytes > CACHE_BUDGET - bytes) {
        slot *old = NULL;
        for (int k = 0; k < SLOTS; k++) {
            slot *candidate = &d->tiles[k];
            if (candidate->tile && candidate->used != d->frame &&
                (!old || candidate->used < old->used)) old = candidate;
        }
        if (!old) return false;
        cache_drop(d, old);
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
        if (r->z < 0) {
            d->tilejson = r->ok && template_from_json(d, r->json) ? 1 : -1;
            if (d->tilejson < 0) d->last_status = r->status;
            free(r->json);
        } else {
            slot *s = NULL;
            for (int k = 0; k < SLOTS; k++) if (d->tiles[k].state == 1 && d->tiles[k].z == r->z && d->tiles[k].x == r->x && d->tiles[k].y == r->y) { s = &d->tiles[k]; break; }
            if (s && s->token == r->token && s->z == r->z && s->x == r->x && s->y == r->y) {
                size_t bytes = cui_mvt_tile_memory_usage(r->tile);
                if (r->ok && cache_make_room(d, bytes)) {
                    s->tile = r->tile; d->cache_bytes += bytes; s->state = 2;
                } else {
                    if (r->ok) d->last_status = 507;
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
    unsigned retry_count = retry ? s->retry_count + 1 : 0;
    cache_drop(d, s);
    d->next_token++;
    *s = slot{z, x, y, 1, d->frame, NULL, d->next_token};
    s->retry_count = retry_count;
    job j = {z, x, y, "", d->next_token}; memcpy(j.path, path, (size_t)n + 1);
    if (!queue_job(d, j)) { *s = slot{}; return; }
}
static void stop(demo *d) {
    EnterCriticalSection(&d->lock); d->stopping = true; LeaveCriticalSection(&d->lock);
    SetEvent(d->stop_event);
    SetEvent(d->available);
    WaitForMultipleObjects(WORKERS, d->threads, TRUE, INFINITE);
    for (int i = 0; i < WORKERS; i++) CloseHandle(d->threads[i]);
    CloseHandle(d->available); CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock);
    for (int i = 0; i < SLOTS; i++) {
        raster_drop(d, &d->tiles[i]);
        cui_mvt_tile_destroy(d->tiles[i].tile);
    }
    free(d->raster_pixels);
    cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw);
    while (d->rcount) {
        result *r = &d->results[d->rhead]; free(r->json); cui_mvt_tile_destroy(r->tile);
        d->rhead = (d->rhead + 1) % RESULT_QUEUE; d->rcount--;
    }
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
    desc.theme = CUI_THEME_DARK;
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
    raster_desc.theme = CUI_THEME_DARK; raster_desc.no_default_fonts = true;
    d->raster_ui = cui_create(&raster_desc);
    d->raster_sw = cui_sw_create();
    if (!d->raster_ui || !d->raster_sw) {
        cui_destroy(d->raster_ui); cui_sw_destroy(d->raster_sw);
        d->raster_ui = NULL; d->raster_sw = NULL;
    }
    d->window = (HWND)cui_app_native_window(app);
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
        CloseHandle(d->available); CloseHandle(d->stop_event); DeleteCriticalSection(&d->lock); cui_app_destroy(app); free(d); return 1;
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
        if (getenv("CUI_MAP_DEMO_BENCH") && d->frame >= 649 && d->frame < 750) {
            cui_input_mouse_move(ui, 500 + ((int)(d->frame - 649) % 50), 300);
            if (d->frame == 649) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
            if (d->frame == 749) cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
        }
        pump(d); d->frame++;
        bool baked = !getenv("CUI_MAP_DEMO_DISABLE_RASTER") && raster_prepare(d);
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
        } else if (d->failures) {
            cui_textf(ui, "%d tiles unavailable%s", d->failures,
                      (d->last_status == 429 || d->last_status == 503) ? " (throttled)" :
                      d->last_status == 507 ? " (cache full)" :
                      d->last_status == 200 ? " (invalid tile)" :
                      d->last_status >= 400 ? " (HTTP error)" : " (network error)");
            if (cui_button_ex(ui, "Retry", nullptr)) {
                for (int i = 0; i < SLOTS; i++) if (d->tiles[i].state == 3) d->tiles[i].state = 0;
                d->failures = 0;
            }
        } else if (d->pending) {
            cui_textf(ui, "Loading %d tiles · %d ready", d->pending, d->loaded);
        } else cui_secondary(ui, "Drag to pan  ·  Wheel to zoom  ·  Arrow keys to move");
        cui_box_end(ui);
        cui_map_opts options = {};
        options.lookup = tile_lookup;
        if (!getenv("CUI_MAP_DEMO_DISABLE_RASTER")) options.raster_lookup = raster_lookup;
        options.request = tile_request; options.tile_user = d;
        options.markers = markers; options.marker_count = 2;
        options.min_zoom = d->min_zoom; options.max_zoom = d->max_zoom ? d->max_zoom : 14;
        cui_map_result hit = cui_map_ex(ui, "online-map", &d->view, &options);
        if (baked) for (int i = 0; i < SLOTS; i++) {
            slot *s = &d->tiles[i];
            if (s->state == 2 && s->used == d->frame && !s->raster &&
                d->raster_bytes <= RASTER_BUDGET - d->raster_tile_bytes) {
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
        cui_textf(ui, "z%d · Cache %.1f/96 MiB · Raster %.1f/48 MiB · H%llu/M%llu",
                  (int)d->view.zoom, (double)d->cache_bytes / (1024 * 1024),
                  (double)d->raster_bytes / (1024 * 1024), d->hits, d->misses);
        bool shown_failure = false;
        for (int i = 0; i < SLOTS; i++) {
            slot *s = &d->tiles[i];
            if (s->state != 3 || s->used != d->frame) continue;
            if (!shown_failure) {
                if (s->status == 200) cui_textf(ui, "Tile z%d/%d/%d: invalid PBF", s->z, s->x, s->y);
                else if (s->status == 507) cui_textf(ui, "Tile z%d/%d/%d: cache full", s->z, s->x, s->y);
                else if (s->status) cui_textf(ui, "Tile z%d/%d/%d: HTTP %u", s->z, s->x, s->y, s->status);
                else cui_textf(ui, "Tile z%d/%d/%d: network error", s->z, s->x, s->y);
                shown_failure = true;
            }
            if (s->next_retry_tick && s->retry_count < 2) {
                int32_t wait_ms = (int32_t)(s->next_retry_tick - GetTickCount());
                if (wait_ms > 0) cui_request_wakeup(ui, (float)wait_ms / 1000.f);
            }
        }
        if (d->selected >= 0) cui_textf(ui, "Marker: %s", markers[d->selected].label);
        cui_box_end(ui);
        cui_box_end(ui);
    }
    stop(d); cui_app_destroy(app); free(d);
    return 0;
}
