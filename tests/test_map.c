#include <cui/cui.h>
#include <cui/cui_render.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../examples/map_fixture.h"
static int failures;
/* A long polygon edge is valid even when its vertex count exceeds the
 * per-scanline crossing budget; only actual crossings consume that budget. */
typedef struct { unsigned char b[65536]; size_t n; } long_pb;
static void long_var(long_pb *p, unsigned v) {
    while (v >= 128) { p->b[p->n++] = (unsigned char)(v | 128); v >>= 7; }
    p->b[p->n++] = (unsigned char)v;
}
static void long_field(long_pb *p, unsigned id, unsigned v) {
    long_var(p, id << 3); long_var(p, v);
}
static void long_nested(long_pb *p, unsigned id, const void *data, size_t n) {
    long_var(p, (id << 3) | 2); long_var(p, (unsigned)n);
    memcpy(p->b + p->n, data, n); p->n += n;
}
static void long_coord(long_pb *p, int v) {
    long_var(p, ((unsigned)v << 1) ^ (unsigned)(v >> 31));
}
static cui_mvt_tile *long_water_tile(void) {
    long_pb geom = {{0}, 0}, feature = {{0}, 0}, layer = {{0}, 0}, tile = {{0}, 0};
    long_var(&geom, 9); long_coord(&geom, 0); long_coord(&geom, 0);
    long_var(&geom, (2102u << 3) | 2); /* 2100 top-edge points, two bottom corners */
    int x = 0;
    for (int i = 0; i < 2100; i++) {
        int next = (i + 1) * 4096 / 2100;
        long_coord(&geom, next - x); long_coord(&geom, 0); x = next;
    }
    long_coord(&geom, 0); long_coord(&geom, 4096);
    long_coord(&geom, -4096); long_coord(&geom, 0);
    long_var(&geom, 15);
    long_field(&feature, 3, 3); long_nested(&feature, 4, geom.b, geom.n);
    long_nested(&layer, 1, "water", 5); long_nested(&layer, 2, feature.b, feature.n);
    long_field(&layer, 5, 4096); long_field(&layer, 15, 2);
    long_nested(&tile, 3, layer.b, layer.n);
    return cui_mvt_tile_decode(tile.b, tile.n);
}

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
typedef struct { cui_mvt_tile *tile; cui_map_view view; int requests; cui_map_result last; } scene;
static void frame(cui_ctx *ui, scene *s);
static cui_mvt_tile *large_place_table_tile(void) {
    long_pb feature = {{0}, 0}, layer = {{0}, 0}, tile = {{0}, 0};
    long_pb value = {{0}, 0}, tags = {{0}, 0}, geom = {{0}, 0};
    long_var(&tags, 0); long_var(&tags, 8192); /* last entry exceeds old 8192 cap */
    long_var(&geom, 9); long_coord(&geom, 0); long_coord(&geom, 0);
    long_nested(&feature, 2, tags.b, tags.n);
    long_field(&feature, 3, 1); long_nested(&feature, 4, geom.b, geom.n);
    long_nested(&layer, 1, "place", 5);
    long_nested(&layer, 2, feature.b, feature.n);
    long_nested(&layer, 3, "name", 4);
    for (int i = 0; i <= 8192; i++) {
        value.n = 0; long_nested(&value, 1, i == 8192 ? "Last" : "A", i == 8192 ? 4 : 1);
        long_nested(&layer, 4, value.b, value.n);
    }
    long_field(&layer, 5, 4096); long_field(&layer, 15, 2);
    long_nested(&tile, 3, layer.b, layer.n);
    return cui_mvt_tile_decode(tile.b, tile.n);
}
/* Exercise the last referenced value in both an earlier provider tile
 * (21,359 values) and z2/1/0 (33,766 values). */
typedef struct { unsigned char b[262144]; size_t n; } large_pb;
static void large_var(large_pb *b, unsigned n) {
    while (n >= 128) { b->b[b->n++] = (unsigned char)(n | 128); n >>= 7; }
    b->b[b->n++] = (unsigned char)n;
}
static void large_nested(large_pb *b, unsigned id, const void *data, size_t n) {
    large_var(b, (id << 3) | 2); large_var(b, (unsigned)n);
    memcpy(b->b + b->n, data, n); b->n += n;
}
static cui_mvt_tile *observed_place_values_tile(unsigned value_count) {
    map_fixture_buf tags = {{0}, 0}, geom = {{0}, 0};
    map_fixture_buf feature = {{0}, 0}, value = {{0}, 0};
    large_pb *layer = malloc(sizeof *layer), *tile = malloc(sizeof *tile);
    if (!layer || !tile) { free(layer); free(tile); return NULL; }
    layer->n = tile->n = 0;
    map_fixture_var(&tags, 0); map_fixture_var(&tags, value_count - 1);
    map_fixture_var(&geom, 9); map_fixture_coord(&geom, 0); map_fixture_coord(&geom, 0);
    map_fixture_nested(&feature, 2, tags.b, tags.n);
    map_fixture_field(&feature, 3, 1); map_fixture_nested(&feature, 4, geom.b, geom.n);
    large_nested(layer, 1, "place", 5);
    large_nested(layer, 2, feature.b, feature.n);
    large_nested(layer, 3, "name", 4);
    for (unsigned i = 0; i < value_count; i++) {
        value.n = 0;
        map_fixture_nested(&value, 1, i == value_count - 1 ? "Last" : "A", i == value_count - 1 ? 4 : 1);
        large_nested(layer, 4, value.b, value.n);
    }
    large_var(layer, 5 << 3); large_var(layer, 4096);
    large_var(layer, 15 << 3); large_var(layer, 2);
    large_nested(tile, 3, layer->b, layer->n);
    cui_mvt_tile *decoded = cui_mvt_tile_decode(tile->b, tile->n);
    free(layer); free(tile);
    return decoded;
}
/* Large key and feature tables must not reject a layer whose only retained
 * place is the final feature, tagged with the final key. */
static cui_mvt_tile *wide_place_tables_tile(void) {
    map_fixture_buf tags = {{0}, 0}, geom = {{0}, 0}, line_geom = {{0}, 0};
    map_fixture_buf feature = {{0}, 0}, ignored = {{0}, 0}, value = {{0}, 0};
    large_pb *layer = malloc(sizeof *layer), *tile = malloc(sizeof *tile);
    if (!layer || !tile) { free(layer); free(tile); return NULL; }
    layer->n = tile->n = 0;
    map_fixture_var(&tags, 16384); map_fixture_var(&tags, 0);
    map_fixture_var(&geom, 9); map_fixture_coord(&geom, 0); map_fixture_coord(&geom, 0);
    map_fixture_nested(&feature, 2, tags.b, tags.n);
    map_fixture_field(&feature, 3, 1); map_fixture_nested(&feature, 4, geom.b, geom.n);
    map_fixture_var(&line_geom, 9); map_fixture_coord(&line_geom, 0); map_fixture_coord(&line_geom, 0);
    map_fixture_var(&line_geom, 10); map_fixture_coord(&line_geom, 1); map_fixture_coord(&line_geom, 0);
    map_fixture_field(&ignored, 3, 2); map_fixture_nested(&ignored, 4, line_geom.b, line_geom.n);
    large_nested(layer, 1, "place", 5);
    for (int i = 0; i < 16384; i++) large_nested(layer, 2, ignored.b, ignored.n);
    large_nested(layer, 2, feature.b, feature.n);
    for (int i = 0; i < 16384; i++) large_nested(layer, 3, "x", 1);
    large_nested(layer, 3, "name", 4);
    map_fixture_nested(&value, 1, "Last", 4);
    large_nested(layer, 4, value.b, value.n);
    large_var(layer, 5 << 3); large_var(layer, 4096);
    large_var(layer, 15 << 3); large_var(layer, 2);
    large_nested(tile, 3, layer->b, layer->n);
    cui_mvt_tile *decoded = cui_mvt_tile_decode(tile->b, tile->n);
    free(layer); free(tile);
    return decoded;
}
static const cui_mvt_tile *bench_lookup(void *user, int z, int x, int y) {
    return z == 8 && x == 137 && y == 83 ? (const cui_mvt_tile *)user : NULL;
}
static void bench_frame(cui_ctx *ui, cui_mvt_tile *tile, cui_map_view *view) {
    cui_begin_frame(ui, 1.f/60);
    cui_map(ui, "bench", view, .lookup = bench_lookup, .tile_user = tile);
    cui_end_frame(ui);
}
static int bench_drag(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return 2;
    unsigned char *data = malloc(2 * 1024 * 1024 + 1);
    if (!data) { fclose(f); return 2; }
    size_t n = fread(data, 1, 2 * 1024 * 1024 + 1, f); fclose(f);
    cui_mvt_tile *tile = cui_mvt_tile_decode(data, n); free(data);
    if (!tile) return 2;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme=CUI_THEME_DARK});
    if (!ui) { cui_mvt_tile_destroy(tile); return 2; }
    cui_set_viewport(ui, 400, 400, 1);
    cui_map_view view = {52.52, 13.405, 8};
    bench_frame(ui, tile, &view); bench_frame(ui, tile, &view);
    int still = cui_get_draw_data(ui)->vertex_count;
    cui_input_mouse_move(ui, 200, 200); bench_frame(ui, tile, &view);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); bench_frame(ui, tile, &view);
    cui_input_mouse_move(ui, 225, 200);
    bench_frame(ui, tile, &view);
    int drag = cui_get_draw_data(ui)->vertex_count;
    fprintf(stderr,"drag benchmark still=%d vertices drag=%d vertices longitude=%.6f\n",still,drag,view.longitude);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); bench_frame(ui, tile, &view);
    cui_destroy(ui); cui_mvt_tile_destroy(tile);
    return drag < still / 2 ? 0 : 1;
}

typedef struct { cui_mvt_tile *tile; cui_texture raster; const cui_map_palette *palette; } raster_test;
static const cui_mvt_tile *raster_test_lookup(void *u, int z, int x, int y) {
    raster_test *test = u;
    return z == 0 && x == 0 && y == 0 ? test->tile : NULL;
}
static cui_texture raster_test_image(void *u, int z, int x, int y) {
    raster_test *test = u;
    return z == 0 && x == 0 && y == 0 ? test->raster : 0;
}
static void raster_test_frame(cui_ctx *ui, raster_test *test, bool cached) {
    cui_map_view view = {0, 0, 0};
    cui_map_opts opts = {0};
    opts.lookup = raster_test_lookup; opts.tile_user = test;
    opts.palette = test->palette;
    if (cached) opts.raster_lookup = raster_test_image;
    cui_begin_frame(ui, 1.f/60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        cui_map_ex(ui, "raster-test", &view, &opts);
    }
    cui_end_frame(ui);
}
static void check_raster_fallback(const map_fixture_buf *raw, int scale,
                                  cui_theme_mode mode, bool custom) {
    cui_mvt_tile *tile = cui_mvt_tile_decode(raw->b, raw->n);
    CHECK(tile != NULL);
    if (!tile) return;
    cui_sw_renderer *off_sw = cui_sw_create(), *main_sw = cui_sw_create();
    cui_ctx_desc desc = {0}; desc.theme = mode; desc.no_default_fonts = true;
    cui_ctx *off = cui_create(&desc), *ui = cui_create(&desc);
    CHECK(off && ui && off_sw && main_sw);
    if (off && ui && off_sw && main_sw) {
        int px = 256 * scale;
        size_t bytes = (size_t)px * px * 4;
        unsigned char *off_px = malloc(bytes), *vector_px = malloc(bytes), *cached_px = malloc(bytes), *image_only_px = malloc(bytes);
        CHECK(off_px && vector_px && cached_px && image_only_px);
        if (off_px && vector_px && cached_px && image_only_px) {
            cui_map_palette palette = cui_map_palette_default(mode);
            if (custom) { palette.land = CUI_RGB(0x514A42); palette.water = CUI_RGB(0x88AABB); }
            cui_set_viewport(off, 256, 256, (float)scale);
            cui_begin_frame(off, 1.f/60);
            cui_map_tile_draw_base_styled(off, tile, (cui_rect){0, 0, 256, 256}, 0,
                                           custom ? &palette : NULL);
            cui_end_frame(off);
            cui_sw_render(off_sw, cui_get_draw_data(off), off_px, px, px, px * 4, CUI_SW_RGBA);
            cui_renderer_iface iface = cui_sw_iface(main_sw);
            cui_set_renderer(ui, &iface);
            cui_set_viewport(ui, 256, 256, (float)scale);
            raster_test test = {tile, 0, custom ? &palette : NULL};
            raster_test_frame(ui, &test, false); raster_test_frame(ui, &test, false);
            int normal_vertices = cui_get_draw_data(ui)->vertex_count;
            cui_sw_render(main_sw, cui_get_draw_data(ui), vector_px, px, px, px * 4, CUI_SW_RGBA);
            test.raster = cui_image_create(ui, px, px, off_px);
            CHECK(test.raster != 0);
            raster_test_frame(ui, &test, true);
            int cached_vertices = cui_get_draw_data(ui)->vertex_count;
            cui_sw_render(main_sw, cui_get_draw_data(ui), cached_px, px, px, px * 4, CUI_SW_RGBA);
            CHECK(cached_vertices < normal_vertices);
            for (int y = 16; y < 240; y += 32) for (int x = 16; x < 240; x += 32) {
                size_t p = (size_t)((y * scale) * px + x * scale) * 4;
                CHECK(memcmp(vector_px + p, cached_px + p, 3) == 0);
            }
            /* A host may discard decoded geometry while retaining its image.
             * The widget must still draw that image, without frozen labels. */
            test.tile = NULL;
            raster_test_frame(ui, &test, true);
            cui_sw_render(main_sw, cui_get_draw_data(ui), image_only_px, px, px, px * 4, CUI_SW_RGBA);
            int matching = 0;
            for (int y = 16; y < 240; y += 32) for (int x = 16; x < 240; x += 32) {
                size_t p = (size_t)((y * scale) * px + x * scale) * 4;
                if (memcmp(cached_px + p, image_only_px + p, 3) == 0) matching++;
            }
            CHECK(matching >= 30); /* labels can disappear with decoded data */
            CHECK(cui_get_draw_data(ui)->vertex_count < normal_vertices);
            cui_image_destroy(ui, test.raster);
        }
        free(off_px); free(vector_px); free(cached_px); free(image_only_px);
    }
    cui_destroy(off); cui_destroy(ui); cui_sw_destroy(off_sw); cui_sw_destroy(main_sw);
    cui_mvt_tile_destroy(tile);
}
typedef struct {
    cui_mvt_tile *tile; cui_texture parent, child;
    cui_map_view view; cui_map_result result;
    int source_requests, target_requests; bool child_ready, child_raster_ready, parent_ready, all_children, defer_base;
} zoom_scene;
static const cui_mvt_tile *zoom_lookup(void *user, int z, int x, int y) {
    zoom_scene *s = user;
    if (z == 0 && x == 0 && y == 0) return s->parent_ready ? s->tile : NULL;
    return s->child_ready && z == 1 && (s->all_children || (x == 0 && y == 0)) ? s->tile : NULL;
}
static cui_texture zoom_image(void *user, int z, int x, int y) {
    zoom_scene *s = user;
    if (z == 0 && x == 0 && y == 0) return s->parent_ready ? s->parent : 0;
    return s->child_raster_ready && z == 1 && (s->all_children || (x == 0 && y == 0)) ? s->child : 0;
}
static void zoom_request(void *user, int z, int x, int y) {
    (void)x; (void)y;
    zoom_scene *s = user;
    if (z == 0) s->source_requests++;
    if (z == 1) s->target_requests++;
}
static void zoom_frame(cui_ctx *ui, zoom_scene *s) {
    cui_map_opts opts = {0}; opts.lookup = zoom_lookup; opts.raster_lookup = zoom_image;
    opts.defer_vector_base = s->defer_base;
    static const cui_map_marker marker[] = {{7, 0, 0, "Center"}};
    opts.markers = marker; opts.marker_count = 1;
    opts.request = zoom_request; opts.tile_user = s;
    cui_begin_frame(ui, 1.f/60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        s->result = cui_map_ex(ui, "zoom-test", &s->view, &opts);
    }
    cui_end_frame(ui);
}
static void check_zoom_fallback(const map_fixture_buf *raw) {
    zoom_scene s = {0}; s.tile = cui_mvt_tile_decode(raw->b, raw->n); s.parent_ready = true; s.defer_base = true;
    cui_ctx_desc desc = {0}; desc.theme = CUI_THEME_DARK;
    cui_ctx *ui = cui_create(&desc);
    cui_sw_renderer *sw = cui_sw_create();
    CHECK(s.tile && ui && sw);
    if (s.tile && ui && sw) {
        cui_renderer_iface iface = cui_sw_iface(sw);
        cui_set_renderer(ui, &iface); cui_set_viewport(ui, 256, 256, 1);
        size_t n = 256u * 256 * 4;
        unsigned char *solid = malloc(n), *screen = malloc(n);
        CHECK(solid && screen);
        if (solid && screen) {
            for (size_t i = 0; i < n; i += 4) {
                solid[i] = 183; solid[i + 1] = 36; solid[i + 2] = 153; solid[i + 3] = 255;
            }
            s.parent = cui_image_create(ui, 256, 256, solid);
            for (size_t i = 0; i < n; i += 4) {
                solid[i] = 24; solid[i + 1] = 174; solid[i + 2] = 231; solid[i + 3] = 255;
            }
            s.child = cui_image_create(ui, 256, 256, solid);
            CHECK(s.parent && s.child);
            zoom_frame(ui, &s); zoom_frame(ui, &s);
            cui_input_mouse_move(ui, 64, 128); zoom_frame(ui, &s);
            cui_input_scroll(ui, 0, 1); zoom_frame(ui, &s);
            CHECK(s.result.view_changed && s.view.zoom == 1);
            CHECK(fabs(s.view.longitude) < 45);
            zoom_frame(ui, &s);
            CHECK(fabs(s.view.longitude) > 0 && fabs(s.view.longitude) < 45);
            for (int i = 0; i < 14; i++) zoom_frame(ui, &s);
            CHECK(fabs(s.view.longitude + 45) < 0.1);
            cui_input_mouse_move(ui, 192, 128); zoom_frame(ui, &s);
            CHECK(s.result.hovered_marker == 7);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            size_t p = ((size_t)220 * 256 + 220) * 4;
            CHECK(screen[p] == 183 && screen[p + 1] == 36 && screen[p + 2] == 153);
            CHECK(s.target_requests > 0 && s.source_requests == 0);
            s.child_ready = true; /* decoded but not yet baked: keep source image */
            zoom_frame(ui, &s);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            p = ((size_t)32 * 256 + 32) * 4;
            CHECK(screen[p] == 183 && screen[p + 1] == 36 && screen[p + 2] == 153);
            s.child_raster_ready = true;
            zoom_frame(ui, &s);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            CHECK(screen[p] == 24 && screen[p + 1] == 174 && screen[p + 2] == 231);
            p = ((size_t)220 * 256 + 220) * 4;
            CHECK(screen[p] == 183 && screen[p + 1] == 36 && screen[p + 2] == 153);
            cui_input_scroll(ui, 0, -1); zoom_frame(ui, &s);
            CHECK(s.view.zoom == 0); /* interrupt animation from current scale */
            zoom_frame(ui, &s);
            cui_input_scroll(ui, 0, 1); zoom_frame(ui, &s);
            CHECK(s.view.zoom == 1); /* retarget without queuing */
            cui_theme_get(ui)->motion = 0;
            zoom_frame(ui, &s);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            p = ((size_t)220 * 256 + 220) * 4;
            CHECK(screen[p] == 183 && screen[p + 1] == 36 && screen[p + 2] == 153);
            s.view.zoom = 0; s.view.longitude = 179.9;
            zoom_frame(ui, &s); /* explicit recenter resets the previous anchor */
            cui_input_mouse_move(ui, 235, 128); zoom_frame(ui, &s);
            cui_input_scroll(ui, 0, 1); zoom_frame(ui, &s);
            CHECK(isfinite(s.view.longitude) && s.view.longitude >= -180 && s.view.longitude < 180);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            p = ((size_t)200 * 256 + 200) * 4;
            CHECK(screen[p] == 183 && screen[p + 1] == 36 && screen[p + 2] == 153);
            cui_image_destroy(ui, s.child); cui_image_destroy(ui, s.parent);
        }
        free(solid); free(screen);
    }
    cui_destroy(ui); cui_sw_destroy(sw); cui_mvt_tile_destroy(s.tile);
}
static void check_zoom_out_fallback(const map_fixture_buf *raw) {
    zoom_scene s = {0}; s.tile = cui_mvt_tile_decode(raw->b, raw->n);
    s.view.zoom = 1; s.child_ready = s.child_raster_ready = s.all_children = true;
    cui_ctx_desc desc = {0}; desc.theme = CUI_THEME_DARK;
    cui_ctx *ui = cui_create(&desc);
    cui_sw_renderer *sw = cui_sw_create();
    CHECK(s.tile && ui && sw);
    if (s.tile && ui && sw) {
        cui_renderer_iface iface = cui_sw_iface(sw);
        cui_set_renderer(ui, &iface); cui_set_viewport(ui, 256, 256, 1);
        size_t n = 256u * 256 * 4;
        unsigned char *solid = malloc(n), *screen = malloc(n);
        CHECK(solid && screen);
        if (solid && screen) {
            for (size_t i = 0; i < n; i += 4) {
                solid[i] = 24; solid[i + 1] = 174; solid[i + 2] = 231; solid[i + 3] = 255;
            }
            s.child = cui_image_create(ui, 256, 256, solid);
            zoom_frame(ui, &s); zoom_frame(ui, &s);
            cui_input_mouse_move(ui, 128, 128); zoom_frame(ui, &s);
            cui_input_scroll(ui, 0, -1); zoom_frame(ui, &s);
            for (int i = 0; i < 15; i++) zoom_frame(ui, &s);
            CHECK(s.view.zoom == 0 && s.source_requests > 0 && s.target_requests == 0);
            cui_sw_render(sw, cui_get_draw_data(ui), screen, 256, 256, 1024, CUI_SW_RGBA);
            size_t p = ((size_t)220 * 256 + 220) * 4;
            CHECK(screen[p] == 24 && screen[p + 1] == 174 && screen[p + 2] == 231);
            cui_image_destroy(ui, s.child);
        }
        free(solid); free(screen);
    }
    cui_destroy(ui); cui_sw_destroy(sw); cui_mvt_tile_destroy(s.tile);
}
/* One named place per tile: a large viewport has more than 64 candidates.
 * The last rows must not disappear simply because top-left tiles were first. */
static const cui_mvt_tile *label_lookup(void *user, int z, int x, int y) {
    (void)x; (void)y;
    return z == 8 ? (const cui_mvt_tile *)user : NULL;
}
static void check_labels_cover_large_view(void) {
    map_fixture_buf geom = {{0}, 0}, tags = {{0}, 0}, feature = {{0}, 0};
    map_fixture_buf value = {{0}, 0}, layer = {{0}, 0}, raw = {{0}, 0};
    map_fixture_var(&geom, 9); map_fixture_coord(&geom, 2048); map_fixture_coord(&geom, 2048);
    map_fixture_var(&tags, 0); map_fixture_var(&tags, 0);
    map_fixture_nested(&feature, 2, tags.b, tags.n);
    map_fixture_field(&feature, 3, 1); map_fixture_nested(&feature, 4, geom.b, geom.n);
    map_fixture_nested(&value, 1, "Town", 4);
    map_fixture_nested(&layer, 1, "place", 5);
    map_fixture_nested(&layer, 2, feature.b, feature.n);
    map_fixture_nested(&layer, 3, "name", 4);
    map_fixture_nested(&layer, 4, value.b, value.n);
    map_fixture_field(&layer, 5, 4096); map_fixture_field(&layer, 15, 2);
    map_fixture_nested(&raw, 3, layer.b, layer.n);
    cui_mvt_tile *tile = cui_mvt_tile_decode(raw.b, raw.n);
    cui_ctx_desc desc = {0}; desc.theme = CUI_THEME_DARK;
    cui_ctx *ui = cui_create(&desc);
    CHECK(tile && ui);
    if (tile && ui) {
        cui_map_view view = {0, 0, 8};
        cui_map_opts opts = {0}; opts.lookup = label_lookup; opts.tile_user = tile;
        cui_set_viewport(ui, 3072, 2048, 1);
        for (int i = 0; i < 2; i++) {
            cui_begin_frame(ui, 1.f / 60);
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
                cui_map_ex(ui, "labels", &view, &opts);
            cui_end_frame(ui);
        }
        const cui_draw_data *draw = cui_get_draw_data(ui);
        int top = 0, bottom_right = 0;
        for (int i = 0; i < draw->vertex_count; i++) {
            const cui_vertex *v = &draw->vertices[i];
            if (v->mode != CUI_SHADE_TEXT) continue;
            if (v->y < 512) top++;
            if (v->y > 1536 && v->x > 2048) bottom_right++;
        }
        CHECK(top > 0 && bottom_right > 0);
        cui_destroy(ui);
    } else cui_destroy(ui);
    cui_mvt_tile_destroy(tile);
}
/* Three labels share a world cell. Panning across an old screen-grid line
 * must not change the interior label count from two to three. */
static void check_labels_stable_across_pan(void) {
    map_fixture_buf geom = {{0}, 0}, tags = {{0}, 0}, feature = {{0}, 0};
    map_fixture_buf value = {{0}, 0}, layer = {{0}, 0}, raw = {{0}, 0};
    map_fixture_var(&tags, 0); map_fixture_var(&tags, 0);
    map_fixture_nested(&layer, 1, "place", 5);
    for (int x = 80; x <= 160; x += 40) {
        geom.n = feature.n = 0;
        map_fixture_var(&geom, 9);
        map_fixture_coord(&geom, x * 16); map_fixture_coord(&geom, 2048);
        map_fixture_nested(&feature, 2, tags.b, tags.n);
        map_fixture_field(&feature, 3, 1);
        map_fixture_nested(&feature, 4, geom.b, geom.n);
        map_fixture_nested(&layer, 2, feature.b, feature.n);
    }
    map_fixture_nested(&value, 1, "T", 1);
    map_fixture_nested(&layer, 3, "name", 4);
    map_fixture_nested(&layer, 4, value.b, value.n);
    map_fixture_field(&layer, 5, 4096); map_fixture_field(&layer, 15, 2);
    map_fixture_nested(&raw, 3, layer.b, layer.n);
    cui_mvt_tile *tile = cui_mvt_tile_decode(raw.b, raw.n);
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_DARK});
    CHECK(tile && ui);
    if (tile && ui) {
        cui_map_opts opts = {0}; opts.lookup = label_lookup; opts.tile_user = tile;
        cui_map_view view = {0, 0, 8};
        cui_set_viewport(ui, 2048, 1024, 1);
        int counts[2] = {0, 0};
        for (int step = 0; step < 2; step++) {
            view.longitude = step ? 120.0 * 360.0 / (256.0 * 256.0) : 0;
            cui_begin_frame(ui, 1.f / 60);
            cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
                cui_map_ex(ui, "stable-labels", &view, &opts);
            cui_end_frame(ui);
            const cui_draw_data *draw = cui_get_draw_data(ui);
            for (int i = 0; i < draw->vertex_count; i++) {
                const cui_vertex *v = &draw->vertices[i];
                if (v->mode == CUI_SHADE_TEXT && v->x > 960 && v->x < 1210 &&
                    v->y > 600 && v->y < 680) counts[step]++;
            }
        }
        CHECK(counts[0] > 0 && counts[0] == counts[1]);
    }
    cui_destroy(ui); cui_mvt_tile_destroy(tile);
}
/* Synthetic z13/z14 source: the optional POI layer intentionally precedes
 * base place geometry, and its crowded variant exceeds the POI candidate cap. */
static cui_mvt_tile *detailed_names_tile(bool crowded, int only_layer) {
    static const char *layers[] = {"poi", "transportation_name", "water_name",
                                   "park", "mountain_peak", "waterway", "place"};
    static const char *names[] = {"Cafe \xe5\x8c\x97\xe4\xba\xac", "Cedar Road", "Canal", "Green Park",
                                  "Summit", "Stream", "Town"};
    long_pb tile = {{0}, 0}, layer = {{0}, 0}, feature = {{0}, 0};
    long_pb geom = {{0}, 0}, tags = {{0}, 0}, value = {{0}, 0};
    long_var(&tags, 0); long_var(&tags, 0);
    for (int i = 0; i < 7; i++) {
        if (only_layer >= 0 && i != only_layer) continue;
        layer.n = feature.n = geom.n = value.n = 0;
        if (i == 1 || i == 2 || i == 5) {
            long_var(&geom, 9); long_coord(&geom, 500); long_coord(&geom, 600 + i * 480);
            long_var(&geom, 10); long_coord(&geom, 2200); long_coord(&geom, 0);
        } else if (i == 3) {
            long_var(&geom, 9); long_coord(&geom, 2200); long_coord(&geom, 2400);
            long_var(&geom, 26); long_coord(&geom, 900); long_coord(&geom, 0);
            long_coord(&geom, 0); long_coord(&geom, 900);
            long_coord(&geom, -900); long_coord(&geom, 0);
            long_var(&geom, 15);
        } else {
            long_var(&geom, 9); long_coord(&geom, 900 + i * 300); long_coord(&geom, 700 + i * 440);
        }
        long_nested(&feature, 2, tags.b, tags.n);
        long_field(&feature, 3, i == 3 ? 3 : (i == 1 || i == 2 || i == 5 ? 2 : 1));
        long_nested(&feature, 4, geom.b, geom.n);
        long_nested(&layer, 1, layers[i], strlen(layers[i]));
        int n = crowded && i == 0 ? 2049 : 1;
        for (int j = 0; j < n; j++) long_nested(&layer, 2, feature.b, feature.n);
        long_nested(&layer, 3, "name", 4);
        long_nested(&value, 1, names[i], strlen(names[i]));
        long_nested(&layer, 4, value.b, value.n);
        long_field(&layer, 5, 4096); long_field(&layer, 15, 2);
        long_nested(&tile, 3, layer.b, layer.n);
    }
    return cui_mvt_tile_decode(tile.b, tile.n);
}
static const cui_mvt_tile *detailed_lookup(void *user, int z, int x, int y) {
    (void)x; (void)y;
    return z == 12 || z == 13 ? (const cui_mvt_tile *)user : NULL;
}
static int detailed_text_vertices(const cui_mvt_tile *tile, int zoom) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_DARK});
    if (!ui) return -1;
    cui_set_viewport(ui, 600, 600, 1);
    cui_map_view view = {0, 0, (float)zoom};
    cui_map_opts opts = {0}; opts.lookup = detailed_lookup; opts.tile_user = (void *)tile;
    for (int i = 0; i < 2; i++) {
        cui_begin_frame(ui, 1.f / 60);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
            cui_map_ex(ui, "detailed-names", &view, &opts);
        cui_end_frame(ui);
    }
    const cui_draw_data *draw = cui_get_draw_data(ui);
    int count = 0;
    for (int i = 0; i < draw->vertex_count; i++)
        if (draw->vertices[i].mode == CUI_SHADE_TEXT) count++;
    cui_destroy(ui);
    return count;
}
static void check_detailed_names(void) {
    cui_mvt_tile *tile = detailed_names_tile(false, -1), *crowded = detailed_names_tile(true, -1);
    CHECK(tile != NULL && crowded != NULL);
    if (tile && crowded) {
        int old_text = detailed_text_vertices(tile, 12);
        int detailed_text = detailed_text_vertices(tile, 13);
        CHECK(old_text > 0 && detailed_text > old_text);
        CHECK(detailed_text_vertices(crowded, 13) >= detailed_text);
    }
    cui_mvt_tile_destroy(tile); cui_mvt_tile_destroy(crowded);
    for (int i = 0; i < 6; i++) {
        cui_mvt_tile *single = detailed_names_tile(false, i);
        CHECK(single != NULL);
        if (single) {
            int old_text = detailed_text_vertices(single, 12);
            int new_text = detailed_text_vertices(single, 13);
            CHECK(old_text == 0 && new_text > 0);
        }
        cui_mvt_tile_destroy(single);
    }
}
/* Many valid but unnamed optional features must not hide the base place.
 * The old fixed-table overflow skipped this layer entirely. */
static void check_optional_label_table_overflow(void) {
    large_pb *layer = malloc(sizeof *layer), *tile = malloc(sizeof *tile);
    if (!layer || !tile) { CHECK(0); free(layer); free(tile); return; }
    layer->n = tile->n = 0;
    map_fixture_buf geom = {{0}, 0}, feature = {{0}, 0}, tags = {{0}, 0}, value = {{0}, 0};
    map_fixture_var(&geom, 9); map_fixture_coord(&geom, 2048); map_fixture_coord(&geom, 2048);
    map_fixture_field(&feature, 3, 1); map_fixture_nested(&feature, 4, geom.b, geom.n);
    large_nested(layer, 1, "poi", 3);
    for (int i = 0; i <= 16384; i++) large_nested(layer, 2, feature.b, feature.n);
    large_var(layer, 5 << 3); large_var(layer, 4096);
    large_var(layer, 15 << 3); large_var(layer, 2);
    large_nested(tile, 3, layer->b, layer->n);
    layer->n = feature.n = 0;
    map_fixture_var(&tags, 0); map_fixture_var(&tags, 0);
    map_fixture_nested(&feature, 2, tags.b, tags.n);
    map_fixture_field(&feature, 3, 1); map_fixture_nested(&feature, 4, geom.b, geom.n);
    large_nested(layer, 1, "place", 5);
    large_nested(layer, 2, feature.b, feature.n);
    large_nested(layer, 3, "name", 4);
    map_fixture_nested(&value, 1, "Town", 4);
    large_nested(layer, 4, value.b, value.n);
    large_var(layer, 5 << 3); large_var(layer, 4096);
    large_var(layer, 15 << 3); large_var(layer, 2);
    large_nested(tile, 3, layer->b, layer->n);
    cui_mvt_tile *decoded = cui_mvt_tile_decode(tile->b, tile->n);
    CHECK(decoded != NULL);
    if (decoded) CHECK(detailed_text_vertices(decoded, 12) > 0);
    cui_mvt_tile_destroy(decoded); free(layer); free(tile);
}
static const cui_mvt_tile *road_lookup(void *user, int z, int x, int y) {
    (void)z; (void)x; (void)y; return (const cui_mvt_tile *)user;
}
static int line_vertices(int zoom, const char *layer, const char *road_class) {
    map_fixture_buf raw = map_fixture_line(layer, road_class);
    cui_mvt_tile *tile = cui_mvt_tile_decode(raw.b, raw.n);
    if (!tile) return -1;
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_DARK});
    if (!ui) { cui_mvt_tile_destroy(tile); return -1; }
    cui_set_viewport(ui, 256, 256, 1);
    cui_map_view view = {0, 0, (float)zoom};
    for (int i = 0; i < 2; i++) {
        cui_begin_frame(ui, 1.f/60);
        cui_map(ui, "road", &view, .lookup = road_lookup, .tile_user = tile);
        cui_end_frame(ui);
    }
    int vertices = cui_get_draw_data(ui)->vertex_count;
    cui_destroy(ui); cui_mvt_tile_destroy(tile);
    return vertices;
}
static int road_vertices(int zoom, const char *road_class) {
    return line_vertices(zoom, "transportation", road_class);
}
static const cui_mvt_tile *lookup(void *user, int z, int x, int y) {
    scene *s = user; return z == 0 && x == 0 && y == 0 ? s->tile : NULL;
}
static void request(void *user, int z, int x, int y) { (void)z; (void)x; (void)y; ((scene *)user)->requests++; }
static void frame(cui_ctx *ui, scene *s) {
    cui_begin_frame(ui, 1.0f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL) {
        static const cui_map_marker marks[] = {{7, 0, 0, "Center"}};
        s->last = cui_map(ui, "map", &s->view, .lookup = lookup, .request = request, .tile_user = s,
                          .markers = marks, .marker_count = 1);
    }
    cui_end_frame(ui);
}
static void check_marker_badge_hit(void) {
    cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_LIGHT});
    CHECK(ui != NULL);
    if (!ui) return;
    cui_set_viewport(ui, 256, 256, 1);
    cui_map_view view = {0, 0, 0};
    static const cui_map_marker badge[] = {{31, 0, 0, NULL, 12}};
    cui_map_opts opts = {0}; opts.markers = badge; opts.marker_count = 1;
    cui_map_result hit = {0};
    for (int i = 0; i < 2; i++) {
        cui_begin_frame(ui, 1.f / 60);
        cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
            hit = cui_map_ex(ui, "badge", &view, &opts);
        cui_end_frame(ui);
    }
    CHECK(cui_get_cursor(ui) == CUI_CURSOR_ARROW);
    cui_sw_renderer *sw = cui_sw_create();
    unsigned char *pixels = malloc(256 * 256 * 4);
    CHECK(sw != NULL && pixels != NULL);
    unsigned char normal[4] = {0};
    size_t sample = ((size_t)128 * 256 + 140) * 4;
    if (sw && pixels) {
        cui_sw_render(sw, cui_get_draw_data(ui), pixels, 256, 256, 256 * 4, CUI_SW_RGBA);
        memcpy(normal, pixels + sample, sizeof normal);
    }
    cui_input_mouse_move(ui, 140, 128);
    cui_begin_frame(ui, 1.f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        hit = cui_map_ex(ui, "badge", &view, &opts);
    cui_end_frame(ui);
    CHECK(hit.hovered_marker == 31 && cui_get_cursor(ui) == CUI_CURSOR_HAND);
    if (sw && pixels) {
        cui_sw_render(sw, cui_get_draw_data(ui), pixels, 256, 256, 256 * 4, CUI_SW_RGBA);
        CHECK(memcmp(normal, pixels + sample, 3) != 0);
    }
    cui_input_mouse_move(ui, 180, 180);
    cui_begin_frame(ui, 1.f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        hit = cui_map_ex(ui, "badge", &view, &opts);
    cui_end_frame(ui);
    CHECK(hit.hovered_marker == -1 && cui_get_cursor(ui) == CUI_CURSOR_ARROW);
    cui_input_mouse_move(ui, 140, 128);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true);
    cui_begin_frame(ui, 1.f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        hit = cui_map_ex(ui, "badge", &view, &opts);
    cui_end_frame(ui);
    cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false);
    cui_begin_frame(ui, 1.f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        hit = cui_map_ex(ui, "badge", &view, &opts);
    cui_end_frame(ui);
    CHECK(hit.clicked_marker == 31);
    static const cui_map_marker single[] = {{32, 0, 0, NULL, 1}};
    opts.markers = single;
    cui_input_mouse_move(ui, 132, 128);
    cui_begin_frame(ui, 1.f / 60);
    cui_column(ui, .width = CUI_FILL, .height = CUI_FILL)
        hit = cui_map_ex(ui, "badge", &view, &opts);
    cui_end_frame(ui);
    CHECK(hit.hovered_marker == 32 && cui_get_cursor(ui) == CUI_CURSOR_HAND);
    free(pixels); cui_sw_destroy(sw); cui_destroy(ui);
}
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "--drag-bench")) return bench_drag(argv[2]);
    if (argc > 1) {
        FILE *file = fopen(argv[1], "rb");
        if (!file) return 2;
        unsigned char *bytes = malloc(2 * 1024 * 1024 + 1);
        if (!bytes) { fclose(file); return 2; }
        size_t n = fread(bytes, 1, 2 * 1024 * 1024 + 1, file);
        bool complete = feof(file) != 0;
        fclose(file);
        cui_mvt_tile *tile = complete ? cui_mvt_tile_decode(bytes, n) : NULL;
        free(bytes);
        fprintf(stderr, "MVT fixture %s: %zu bytes, %s\n", argv[1], n, tile ? "valid" : "invalid/over limit");
        int result = tile ? 0 : 2;
        cui_mvt_tile_destroy(tile);
        return result;
    }
    map_fixture_buf b = map_fixture();
    cui_mvt_tile *t = cui_mvt_tile_decode(b.b, b.n);
    CHECK(t != NULL);
    CHECK(cui_mvt_tile_memory_usage(NULL) == 0);
    if (t) CHECK(cui_mvt_tile_memory_usage(t) >= b.n + sizeof(void *) * 5);
    CHECK(cui_mvt_tile_decode(NULL, 0) == NULL);
    CHECK(cui_mvt_tile_decode(b.b, 0) == NULL);
    CHECK(cui_mvt_tile_decode(b.b, 2 * 1024 * 1024 + 1) == NULL);
    for (size_t i = 0; i < b.n; i++) { cui_mvt_tile *part = cui_mvt_tile_decode(b.b, i); CHECK(part == NULL); cui_mvt_tile_destroy(part); }
    for (size_t i = 0; i < b.n; i++) {
        unsigned char old = b.b[i]; b.b[i] ^= 0xff;
        cui_mvt_tile *mutated = cui_mvt_tile_decode(b.b, b.n);
        cui_mvt_tile_destroy(mutated); b.b[i] = old;
    }
    /* Owned decode remains usable after the caller reuses its input buffer. */
    b.b[0] = 0;
    unsigned char bad[] = {0x1a, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80};
    CHECK(cui_mvt_tile_decode(bad, sizeof bad) == NULL);
    if (t) {
        cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_LIGHT});
        CHECK(ui != NULL);
        if (ui) {
            scene s = {t, {0, 0, 0}, 0};
            cui_set_viewport(ui, 256, 256, 1);
            frame(ui, &s); frame(ui, &s);
            double wx = -1, wy = -1;
            cui_vec2 projected = {-1, -1};
            CHECK(cui_map_world_point(0, 0, 0, &wx, &wy) && wx == 128 && wy == 128);
            double lat = 1, lon = 1;
            CHECK(cui_map_geo_from_world(0, wx, wy, &lat, &lon));
            CHECK(fabs(lat) < 0.00001 && fabs(lon) < 0.00001);
            CHECK(!cui_map_world_point(15, 0, 0, &wx, &wy));
            CHECK(!cui_map_world_point(0, 91, 0, &wx, &wy));
            CHECK(cui_map_project(&s.last.projection, 0, 0, &projected));
            CHECK(fabs(projected.x - 128) < 0.1 && fabs(projected.y - 128) < 0.1);
            cui_map_projection cropped = s.last.projection;
            cropped.rect.w = cropped.rect.h = 30;
            CHECK(!cui_map_project(&cropped, 45, 0, &projected));
            const cui_draw_data *dd = cui_get_draw_data(ui);
            unsigned char pixels[256 * 256 * 4] = {0};
            cui_sw_renderer *sw = cui_sw_create();
            cui_sw_render(sw, dd, pixels, 256, 256, 256 * 4, CUI_SW_RGBA);
            size_t a = (32 * 256 + 32) * 4, hole = (128 * 256 + 128) * 4;
            CHECK(memcmp(pixels + a, pixels + hole, 3) != 0);
            cui_sw_destroy(sw);
            CHECK(s.requests == 0);
            cui_input_mouse_move(ui, 128, 128); frame(ui, &s);
            CHECK(s.last.hovered_marker == 7);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, &s);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &s);
            CHECK(s.last.clicked_marker == 7);
            cui_input_mouse_move(ui, 155, 128); frame(ui, &s);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, &s);
            cui_input_mouse_move(ui, 185, 128); frame(ui, &s);
            CHECK(s.last.view_changed && fabs(s.view.longitude) > 1);
            CHECK(cui_map_project(&s.last.projection, 0, 0, &projected));
            CHECK(fabs(projected.x - 158) < 0.1 && fabs(projected.y - 128) < 0.1);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &s);
            cui_input_scroll(ui, 0, 1); frame(ui, &s);
            CHECK(s.view.zoom == 1 && s.last.projection.animating);
            double before_key = s.view.longitude;
            cui_input_key(ui, CUI_KEY_RIGHT, true, 0); frame(ui, &s);
            CHECK(s.last.view_changed && s.view.longitude != before_key);
            cui_input_key(ui, CUI_KEY_RIGHT, false, 0); frame(ui, &s);
            s.view.longitude = 179.9;
            frame(ui, &s); CHECK(fabs(s.view.longitude - 179.9) < 0.001);
            s.view.latitude = 90; frame(ui, &s);
            CHECK(isfinite(s.view.latitude) && s.view.latitude > 85 && s.view.latitude <= 85.05113);
            cui_destroy(ui);
        }
        cui_mvt_tile_destroy(t);
    }
    /* During a drag, detail is deferred so mouse movement can be painted
     * promptly; releasing restores full tile geometry. */
    map_fixture_buf land = map_fixture_layer("landcover");
    cui_mvt_tile *park = cui_mvt_tile_decode(land.b, land.n);
    CHECK(park != NULL);
    if (park) {
        cui_ctx *ui = cui_create(&(cui_ctx_desc){.theme = CUI_THEME_DARK});
        CHECK(ui != NULL);
        if (ui) {
            scene drag = {park, {0, 0, 0}, 0};
            cui_set_viewport(ui, 256, 256, 1);
            frame(ui, &drag); frame(ui, &drag);
            int still_vertices = cui_get_draw_data(ui)->vertex_count;
            cui_input_mouse_move(ui, 128, 128); frame(ui, &drag);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, true); frame(ui, &drag);
            cui_input_mouse_move(ui, 140, 128); frame(ui, &drag);
            int moving_vertices = cui_get_draw_data(ui)->vertex_count;
            CHECK(drag.last.view_changed && fabs(drag.view.longitude + 16.875) < 0.001);
            CHECK(moving_vertices < still_vertices);
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &drag);
            CHECK(cui_get_draw_data(ui)->vertex_count >= still_vertices);
            cui_destroy(ui);
        }
        cui_mvt_tile_destroy(park);
    }
    map_fixture_buf base_fixture = map_fixture();
    cui_map_palette light = cui_map_palette_default(CUI_THEME_LIGHT);
    cui_map_palette dark = cui_map_palette_default(CUI_THEME_DARK);
    CHECK(light.land.r > dark.land.r && light.place_text.r < dark.place_text.r);
    CHECK(light.water.r != dark.water.r && light.road_major.r != dark.road_major.r);
    CHECK(light.marker_halo.r >= 200 && light.marker_halo.r < 255);
    CHECK(dark.marker_halo.r >= 200 && dark.marker_halo.r < 255);
    check_raster_fallback(&base_fixture, 1, CUI_THEME_LIGHT, false);
    check_raster_fallback(&base_fixture, 2, CUI_THEME_DARK, false);
    check_raster_fallback(&base_fixture, 1, CUI_THEME_DARK, true);
    check_zoom_fallback(&base_fixture);
    check_zoom_out_fallback(&base_fixture);
    check_labels_cover_large_view();
    check_labels_stable_across_pan();
    check_detailed_names();
    check_optional_label_table_overflow();
    check_marker_badge_hit();
    cui_mvt_tile *long_tile = long_water_tile();
    CHECK(long_tile != NULL);
    cui_mvt_tile_destroy(long_tile);
    cui_mvt_tile *large_values = large_place_table_tile();
    CHECK(large_values != NULL);
    cui_mvt_tile_destroy(large_values);
    cui_mvt_tile *observed_values = observed_place_values_tile(21359);
    CHECK(observed_values != NULL);
    cui_mvt_tile_destroy(observed_values);
    cui_mvt_tile *failed_coordinate_values = observed_place_values_tile(33766);
    CHECK(failed_coordinate_values != NULL);
    if (failed_coordinate_values) CHECK(detailed_text_vertices(failed_coordinate_values, 12) > 0);
    cui_mvt_tile_destroy(failed_coordinate_values);
    cui_mvt_tile *wide_tables = wide_place_tables_tile();
    CHECK(wide_tables != NULL);
    if (wide_tables) CHECK(detailed_text_vertices(wide_tables, 12) > 0);
    cui_mvt_tile_destroy(wide_tables);
    CHECK(road_vertices(5, "motorway") > road_vertices(5, "trunk"));
    CHECK(road_vertices(6, "trunk") > road_vertices(6, "primary"));
    CHECK(road_vertices(8, "trunk") > road_vertices(8, "primary"));
    CHECK(road_vertices(9, "primary") > road_vertices(9, "secondary"));
    CHECK(road_vertices(10, "primary") > road_vertices(10, "secondary"));
    CHECK(road_vertices(11, "secondary") > road_vertices(11, "residential"));
    CHECK(road_vertices(12, "secondary") > road_vertices(12, "residential"));
    CHECK(road_vertices(13, "residential") > road_vertices(12, "residential"));
    CHECK(line_vertices(10, "waterway", "stream") > line_vertices(9, "waterway", "stream"));
    fprintf(stderr, "map tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
