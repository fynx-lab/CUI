#include <cui/cui.h>
#include <cui/cui_render.h>
#include <math.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../examples/map_fixture.h"
static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); failures++; } } while (0)
typedef struct { cui_mvt_tile *tile; cui_map_view view; int requests; cui_map_result last; } scene;
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

static const cui_mvt_tile *road_lookup(void *user, int z, int x, int y) {
    (void)z; (void)x; (void)y; return (const cui_mvt_tile *)user;
}
static int road_vertices(int zoom, const char *road_class) {
    map_fixture_buf raw = map_fixture_road(road_class);
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
            cui_input_mouse_button(ui, CUI_MOUSE_LEFT, false); frame(ui, &s);
            cui_input_scroll(ui, 0, 1); frame(ui, &s);
            CHECK(s.view.zoom == 1);
            double before_key = s.view.longitude;
            cui_input_key(ui, CUI_KEY_RIGHT, true, 0); frame(ui, &s);
            CHECK(s.last.view_changed && s.view.longitude != before_key);
            cui_input_key(ui, CUI_KEY_RIGHT, false, 0); frame(ui, &s);
            s.view.longitude = 179.9;
            frame(ui, &s); CHECK(s.view.longitude <= 180 && s.view.longitude >= -180);
            s.view.latitude = 90; frame(ui, &s); CHECK(isfinite(s.view.latitude) && s.view.latitude <= 85.05113);
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
    CHECK(road_vertices(6, "motorway") > road_vertices(6, "primary"));
    CHECK(road_vertices(8, "primary") > road_vertices(8, "secondary"));
    CHECK(road_vertices(10, "secondary") > road_vertices(10, "residential"));
    CHECK(road_vertices(11, "residential") > road_vertices(10, "residential"));
    fprintf(stderr, "map tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
