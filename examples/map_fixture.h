/* Small generated MVT 2.1 fixtures: a polygon with an interior hole and
 * class-tagged road lines. Used by offline scenes and decoder/render tests. */
#ifndef CUI_EXAMPLE_MAP_FIXTURE_H
#define CUI_EXAMPLE_MAP_FIXTURE_H
#include <stdint.h>
#include <string.h>

typedef struct { unsigned char b[512]; size_t n; } map_fixture_buf;
static void map_fixture_var(map_fixture_buf *b, unsigned n) {
    while (n >= 128) { b->b[b->n++] = (unsigned char)(n | 128); n >>= 7; }
    b->b[b->n++] = (unsigned char)n;
}
static void map_fixture_field(map_fixture_buf *b, unsigned id, unsigned value) {
    map_fixture_var(b, id << 3); map_fixture_var(b, value);
}
static void map_fixture_nested(map_fixture_buf *b, unsigned id, const void *data, size_t n) {
    map_fixture_var(b, (id << 3) | 2); map_fixture_var(b, (unsigned)n);
    memcpy(b->b + b->n, data, n); b->n += n;
}
static void map_fixture_coord(map_fixture_buf *b, int x) {
    map_fixture_var(b, ((unsigned)x << 1) ^ (unsigned)(x >> 31));
}
static map_fixture_buf map_fixture_layer(const char *name) {
    map_fixture_buf g = {{0}, 0}, f = {{0}, 0}, layer = {{0}, 0}, tile = {{0}, 0};
    map_fixture_var(&g, 9); map_fixture_coord(&g, 0); map_fixture_coord(&g, 0);
    map_fixture_var(&g, 26); map_fixture_coord(&g, 4096); map_fixture_coord(&g, 0);
    map_fixture_coord(&g, 0); map_fixture_coord(&g, 4096);
    map_fixture_coord(&g, -4096); map_fixture_coord(&g, 0);
    map_fixture_var(&g, 15);
    map_fixture_var(&g, 9); map_fixture_coord(&g, 1024); map_fixture_coord(&g, -3072);
    map_fixture_var(&g, 26); map_fixture_coord(&g, 0); map_fixture_coord(&g, 2048);
    map_fixture_coord(&g, 2048); map_fixture_coord(&g, 0);
    map_fixture_coord(&g, 0); map_fixture_coord(&g, -2048);
    map_fixture_var(&g, 15);
    map_fixture_field(&f, 3, 3); map_fixture_nested(&f, 4, g.b, g.n);
    map_fixture_nested(&layer, 1, name, strlen(name));
    map_fixture_nested(&layer, 2, f.b, f.n);
    map_fixture_field(&layer, 5, 4096); map_fixture_field(&layer, 15, 2);
    map_fixture_nested(&tile, 3, layer.b, layer.n);
    return tile;
}
static map_fixture_buf map_fixture(void) { return map_fixture_layer("water"); }
static map_fixture_buf map_fixture_line(const char *layer_name, const char *road_class) {
    map_fixture_buf geom = {{0}, 0}, feature = {{0}, 0}, value = {{0}, 0};
    map_fixture_buf layer = {{0}, 0}, tile = {{0}, 0}, tags = {{0}, 0};
    map_fixture_var(&geom, 9); map_fixture_coord(&geom, 0); map_fixture_coord(&geom, 0);
    map_fixture_var(&geom, 10); map_fixture_coord(&geom, 4096); map_fixture_coord(&geom, 4096);
    map_fixture_var(&tags, 0); map_fixture_var(&tags, 0);
    map_fixture_nested(&feature, 2, tags.b, tags.n);
    map_fixture_field(&feature, 3, 2);
    map_fixture_nested(&feature, 4, geom.b, geom.n);
    map_fixture_nested(&value, 1, road_class, strlen(road_class));
    map_fixture_nested(&layer, 1, layer_name, strlen(layer_name));
    map_fixture_nested(&layer, 2, feature.b, feature.n);
    map_fixture_nested(&layer, 3, "class", 5);
    map_fixture_nested(&layer, 4, value.b, value.n);
    map_fixture_field(&layer, 5, 4096); map_fixture_field(&layer, 15, 2);
    map_fixture_nested(&tile, 3, layer.b, layer.n);
    return tile;
}
static map_fixture_buf map_fixture_road(const char *road_class) {
    return map_fixture_line("transportation", road_class);
}
#endif
