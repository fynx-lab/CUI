#include "cui_internal.h"
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* Limits apply to uncompressed, untrusted tile data. No CUI context is used
 * during decode, so the host can parse on a worker before handing off a tile. */
enum { MAP_BYTES = 2 * 1024 * 1024, MAP_FEATURES = 12000,
       MAP_POINTS = 200000, MAP_PATHS = 24000,
       MAP_SPANS = 100000, MAP_REFS = 256,
       MAP_LABELS = 256, MAP_LABELS_LOW = 192,
       MAP_LABEL_CELL = 192, MAP_LABEL_CELL_SLOTS = 512,
       MAP_POI_CANDIDATES = 2048, MAP_ROAD_NAME_CANDIDATES = 512,
       MAP_WATER_NAME_CANDIDATES = 256, MAP_PARK_CANDIDATES = 128,
       MAP_PEAK_CANDIDATES = 128, MAP_WATERWAY_CANDIDATES = 256,
       MAP_CROSSINGS = 2048 };
typedef struct { const uint8_t *p; size_t n; } map_slice;
typedef struct { const uint8_t *p, *end; } map_pb;
typedef struct { int32_t x, y; } map_point;
typedef struct { int first, count; } map_path;
typedef struct { float left, right; uint16_t row, height; } map_span;
typedef struct { int kind, first, count, extent, span_first, span_count; map_point anchor; char name[96], class_name[32]; } map_feature;
struct cui_mvt_tile {
    uint8_t *bytes;
    size_t byte_count;
    map_feature *features;
    map_path *paths;
    map_point *points;
    map_span *spans;
    int nfeatures, npaths, npoints, nspans;
    int cfeatures, cpaths, cpoints, cspans;
    bool geometry_limit_hit;
};

static bool map_var(map_pb *p, uint64_t *out) {
    uint64_t n = 0;
    for (int i = 0; i < 10; i++) {
        if (p->p == p->end) return false;
        uint8_t b = *p->p++;
        if (i == 9 && b > 1) return false;
        n |= (uint64_t)(b & 127) << (7 * i);
        if (!(b & 128)) { *out = n; return true; }
    }
    return false;
}
static bool map_field(map_pb *p, unsigned *id, unsigned *wire) {
    uint64_t key;
    if (!map_var(p, &key) || !key || key >> 3 > 536870911) return false;
    *id = (unsigned)(key >> 3); *wire = (unsigned)(key & 7);
    return true;
}
static bool map_bytes(map_pb *p, map_slice *v) {
    uint64_t len;
    if (!map_var(p, &len) || len > (uint64_t)(p->end - p->p)) return false;
    v->p = p->p; v->n = (size_t)len; p->p += len;
    return true;
}
static bool map_skip(map_pb *p, unsigned wire) {
    uint64_t ignored; map_slice v;
    if (wire == 0) return map_var(p, &ignored);
    if (wire == 2) return map_bytes(p, &v);
    if (wire == 1 || wire == 5) {
        size_t n = wire == 1 ? 8 : 4;
        if ((size_t)(p->end - p->p) < n) return false;
        p->p += n; return true;
    }
    return false; /* protobuf groups are not part of MVT */
}
static bool map_eq(map_slice v, const char *s) {
    size_t n = strlen(s);
    return v.n == n && !memcmp(v.p, s, n);
}
static void map_copy(char *dst, size_t cap, map_slice v) {
    size_t n = v.n < cap - 1 ? v.n : cap - 1;
    memcpy(dst, v.p, n); dst[n] = 0;
}
static bool map_grow(void **p, int *cap, int needed, int limit, size_t stride) {
    if (needed > limit) return false;
    if (needed <= *cap) return true;
    int count = *cap ? *cap : 32;
    while (count < needed) count = count > limit / 2 ? limit : count * 2;
    void *next = realloc(*p, (size_t)count * stride);
    if (!next) return false;
    *p = next; *cap = count; return true;
}
static bool map_add_point(cui_mvt_tile *t, int32_t x, int32_t y) {
    if (t->npoints == MAP_POINTS) { t->geometry_limit_hit = true; return false; }
    if (!map_grow((void **)&t->points, &t->cpoints, t->npoints + 1, MAP_POINTS, sizeof *t->points)) return false;
    t->points[t->npoints++] = (map_point){x, y}; return true;
}
static bool map_add_path(cui_mvt_tile *t) {
    if (t->npaths == MAP_PATHS) { t->geometry_limit_hit = true; return false; }
    if (!map_grow((void **)&t->paths, &t->cpaths, t->npaths + 1, MAP_PATHS, sizeof *t->paths)) return false;
    t->paths[t->npaths++] = (map_path){t->npoints, 0}; return true;
}
static bool map_geometry(cui_mvt_tile *t, map_slice data, int type, map_feature *f) {
    map_pb p = {data.p, data.p + data.n};
    int64_t x = 0, y = 0;
    bool closed = false;
    f->first = t->npaths;
    while (p.p < p.end) {
        uint64_t command;
        if (!map_var(&p, &command) || !command || command >> 3 > MAP_POINTS) return false;
        int op = (int)(command & 7), n = (int)(command >> 3);
        if ((op != 1 && op != 2 && op != 7) || n == 0) return false;
        if (op == 7) {
            if (type != 3 || n != 1 || closed || t->npaths == f->first || t->paths[t->npaths - 1].count < 3) return false;
            closed = true; continue;
        }
        if (op == 2 && (t->npaths == f->first || closed || type == 1)) return false;
        if (type == 3 && op == 1 && (n != 1 || (t->npaths != f->first && !closed))) return false;
        if (op == 1) closed = false;
        for (int i = 0; i < n; i++) {
            uint64_t ax, ay;
            if (!map_var(&p, &ax) || !map_var(&p, &ay) || ax > UINT32_MAX || ay > UINT32_MAX) return false;
            x += (int32_t)((uint32_t)(ax >> 1) ^ (uint32_t)-(int)(ax & 1));
            y += (int32_t)((uint32_t)(ay >> 1) ^ (uint32_t)-(int)(ay & 1));
            if (x < -65536 || x > 65536 || y < -65536 || y > 65536) return false;
            if (op == 1 && (type == 1 || i == 0) && !map_add_path(t)) return false;
            if (op == 1 && type != 1 && i != 0) return false;
            if (!map_add_point(t, (int32_t)x, (int32_t)y)) return false;
            t->paths[t->npaths - 1].count++;
        }
    }
    f->count = t->npaths - f->first;
    return f->count > 0 && (type != 3 || closed);
}
static map_point map_name_anchor(const cui_mvt_tile *t, const map_feature *f, int type) {
    map_path first = t->paths[f->first];
    map_point anchor = t->points[first.first];
    if (type == 1) return anchor;
    if (type == 2) {
        int64_t longest = -1;
        for (int j = 0; j < f->count; j++) {
            map_path path = t->paths[f->first + j];
            for (int k = 1; k < path.count; k++) {
                map_point a = t->points[path.first + k - 1], b = t->points[path.first + k];
                int64_t dx = (int64_t)b.x - a.x, dy = (int64_t)b.y - a.y;
                int64_t length = dx * dx + dy * dy;
                if (length > longest) {
                    longest = length;
                    anchor = (map_point){(int32_t)(((int64_t)a.x + b.x) / 2),
                                         (int32_t)(((int64_t)a.y + b.y) / 2)};
                }
            }
        }
    } else if (type == 3) {
        int32_t left = anchor.x, right = anchor.x, top = anchor.y, bottom = anchor.y;
        for (int j = 0; j < f->count; j++) {
            map_path path = t->paths[f->first + j];
            for (int k = 0; k < path.count; k++) {
                map_point p = t->points[path.first + k];
                if (p.x < left) left = p.x; if (p.x > right) right = p.x;
                if (p.y < top) top = p.y; if (p.y > bottom) bottom = p.y;
            }
        }
        anchor = (map_point){(int32_t)(((int64_t)left + right) / 2),
                             (int32_t)(((int64_t)top + bottom) / 2)};
    }
    return anchor;
}
static bool map_value(map_slice encoded, map_slice *out) {
    map_pb p = {encoded.p, encoded.p + encoded.n};
    out->p = (const uint8_t *)""; out->n = 0;
    while (p.p < p.end) {
        unsigned id, wire;
        if (!map_field(&p, &id, &wire)) return false;
        if (id == 1 && wire == 2) { if (!map_bytes(&p, out)) return false; }
        else if (!map_skip(&p, wire)) return false;
    }
    return true;
}
static bool map_feature_parse(cui_mvt_tile *t, map_slice data, int kind, int extent,
                              const map_slice *keys, int nk, const map_slice *values, int nv) {
    map_pb p = {data.p, data.p + data.n}; map_slice tags = {0}, geom = {0};
    int type = 0; map_feature f; memset(&f, 0, sizeof f);
    f.kind = kind; f.extent = extent;
    while (p.p < p.end) {
        unsigned id, wire; uint64_t v;
        if (!map_field(&p, &id, &wire)) return false;
        if (id == 2 && wire == 2) { if (!map_bytes(&p, &tags)) return false; }
        else if (id == 3 && wire == 0) { if (!map_var(&p, &v) || v > 3) return false; type = (int)v; }
        else if (id == 4 && wire == 2) { if (!map_bytes(&p, &geom)) return false; }
        else if (!map_skip(&p, wire)) return false;
    }
    if (!type || !geom.p) return false;
    map_pb tag = {tags.p, tags.p ? tags.p + tags.n : tags.p};
    while (tag.p && tag.p < tag.end) {
        uint64_t k, v;
        if (!map_var(&tag, &k) || !map_var(&tag, &v) || k >= (uint64_t)nk || v >= (uint64_t)nv) return false;
        if (map_eq(keys[k], "name")) map_copy(f.name, sizeof f.name, values[v]);
        if (map_eq(keys[k], "class")) map_copy(f.class_name, sizeof f.class_name, values[v]);
    }
    /* Other geometry types in a recognized layer do not contribute to this
     * renderer, but their protobuf envelopes and tags have been checked. */
    if ((kind == 1 || kind == 4) && type != 3) return true;
    if ((kind == 2 || kind == 5) && type != 2) return true;
    if (kind == 3 && type != 1) return true;
    /* Optional name layers can supply a point or line for the same kind of
     * feature (observed at z13/z14); anchor by actual geometry, not layer. */
    if (kind >= 6 && !f.name[0]) return true;
    if (!map_grow((void **)&t->features, &t->cfeatures, t->nfeatures + 1, MAP_FEATURES, sizeof *t->features)) return false;
    if (!map_geometry(t, geom, type, &f)) return false;
    if (f.name[0] && (kind == 3 || kind >= 5)) f.anchor = map_name_anchor(t, &f, type);
    t->features[t->nfeatures++] = f;
    return true;
}
static bool map_layer(cui_mvt_tile *t, map_slice encoded, bool optional_pass) {
    map_pb p = {encoded.p, encoded.p + encoded.n};
    map_slice name = {0}; int extent = 4096, version = 1;
    size_t feature_count = 0, key_count = 0, value_count = 0;
    /* Discover the layer and count its metadata before allocating. Unknown
     * provider layers are validated but need no metadata table. */
    while (p.p < p.end) {
        unsigned id, wire; uint64_t num; map_slice v;
        if (!map_field(&p, &id, &wire)) return false;
        if (id == 1 && wire == 2) { if (!map_bytes(&p, &name)) return false; }
        else if (id == 5 && wire == 0) { if (!map_var(&p, &num) || num < 256 || num > 16384) return false; extent = (int)num; }
        else if (id == 15 && wire == 0) { if (!map_var(&p, &num) || num != 2) return false; version = (int)num; }
        else if (id == 2 && wire == 2) { if (!map_bytes(&p, &v)) return false; feature_count++; }
        else if (id == 3 && wire == 2) { if (!map_bytes(&p, &v)) return false; key_count++; }
        else if (id == 4 && wire == 2) { if (!map_bytes(&p, &v)) return false; value_count++; }
        else if (!map_skip(&p, wire)) return false;
    }
    if (version != 2 || !name.p) return false;
    int kind = map_eq(name, "water") ? 1 : map_eq(name, "transportation") ? 2 :
               map_eq(name, "place") ? 3 : map_eq(name, "landcover") ? 4 :
               map_eq(name, "waterway") ? 5 : map_eq(name, "poi") ? 6 :
               map_eq(name, "transportation_name") ? 7 : map_eq(name, "water_name") ? 8 :
               map_eq(name, "park") ? 9 : map_eq(name, "mountain_peak") ? 10 : 0;
    if (!kind || (kind >= 6) != optional_pass) return true;
    /* Every counted field takes at least two bytes in the capped input. The
     * temporary slice table is therefore at most 8x the 2 MiB tile size. */
    if (key_count > SIZE_MAX - feature_count ||
        value_count > SIZE_MAX - feature_count - key_count) return false;
    size_t count = feature_count + key_count + value_count;
    if (count > SIZE_MAX / sizeof(map_slice)) return false;
    map_slice *fields = count ? malloc(count * sizeof *fields) : NULL;
    if (count && !fields) return false;
    map_slice *features = fields;
    map_slice *keys = fields ? fields + feature_count : NULL;
    map_slice *values = fields ? fields + feature_count + key_count : NULL;
    size_t nf = 0, nk = 0, nv = 0; bool ok = true;
    bool optional = kind >= 6;
    p = (map_pb){encoded.p, encoded.p + encoded.n};
    while (p.p < p.end) {
        unsigned id, wire; map_slice v;
        if (!map_field(&p, &id, &wire)) { ok = false; break; }
        if (id == 2 && wire == 2) {
            if (!map_bytes(&p, &v)) { ok = false; break; }
            features[nf++] = v;
        } else if (id == 3 && wire == 2) {
            if (!map_bytes(&p, &v)) { ok = false; break; }
            keys[nk++] = v;
        } else if (id == 4 && wire == 2) {
            if (!map_bytes(&p, &v) || !map_value(v, &values[nv++])) { ok = false; break; }
        } else if (!map_skip(&p, wire)) { ok = false; break; }
    }
    int cap = kind == 6 ? MAP_POI_CANDIDATES : kind == 7 ? MAP_ROAD_NAME_CANDIDATES :
              kind == 8 ? MAP_WATER_NAME_CANDIDATES : kind == 9 ? MAP_PARK_CANDIDATES :
              kind == 10 ? MAP_PEAK_CANDIDATES : MAP_FEATURES;
    if (ok) {
        int retained = 0;
        for (size_t i = 0; i < nf; i++) {
            if (optional && (retained >= cap || t->nfeatures >= MAP_FEATURES)) break;
            int before = t->nfeatures, points = t->npoints, paths = t->npaths;
            if (!map_feature_parse(t, features[i], kind, extent, keys, (int)nk, values, (int)nv)) {
                if (optional && t->geometry_limit_hit) {
                    t->nfeatures = before; t->npoints = points; t->npaths = paths;
                    t->geometry_limit_hit = false;
                    break; /* optional names cannot crowd out valid base layers */
                }
                ok = false; break;
            }
            retained += t->nfeatures - before;
        }
    }
    free(fields);
    return ok;
}

static int map_compare_float(const void *a, const void *b) {
    float x = *(const float *)a, y = *(const float *)b;
    return (x > y) - (x < y);
}
static bool map_build_spans(cui_mvt_tile *t, map_feature *f) {
    float xs[MAP_CROSSINGS];
    f->span_first = t->nspans;
    for (int row = 0; row < 256; row++) {
        int n = 0; float y = (row + 0.5f) * f->extent / 256.0f;
        for (int j = 0; j < f->count; j++) {
            map_path path = t->paths[f->first + j];
            /* A ring may contain thousands of points yet cross this scanline
             * only a few times. Bound the actual crossings, not ring length. */
            for (int k = 0; k < path.count; k++) {
                map_point a = t->points[path.first + k], b = t->points[path.first + (k + 1) % path.count];
                if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y)) {
                    if (n == MAP_CROSSINGS) return false;
                    xs[n++] = a.x + (y - a.y) * (b.x - a.x) / (float)(b.y - a.y);
                }
            }
        }
        qsort(xs, (size_t)n, sizeof *xs, map_compare_float);
        for (int k = 1; k < n; k += 2) {
            float left = xs[k - 1] * 256.0f / f->extent, right = xs[k] * 256.0f / f->extent;
            if (right <= left) continue;
            /* Long straight spans collapse into one rectangle. */
            if (k == 1 && n == 2 && t->nspans > f->span_first) {
                map_span *prev = &t->spans[t->nspans - 1];
                if (prev->row + prev->height == row && fabsf(prev->left - left) < 0.01f &&
                    fabsf(prev->right - right) < 0.01f) { prev->height++; continue; }
            }
            if (!map_grow((void **)&t->spans, &t->cspans, t->nspans + 1, MAP_SPANS, sizeof *t->spans)) return false;
            t->spans[t->nspans++] = (map_span){left, right, (uint16_t)row, 1};
        }
    }
    f->span_count = t->nspans - f->span_first;
    return true;
}

cui_mvt_tile *cui_mvt_tile_decode(const void *bytes, size_t size) {
    if (!bytes || !size || size > MAP_BYTES) return NULL;
    cui_mvt_tile *t = calloc(1, sizeof *t);
    if (!t) return NULL;
    t->bytes = malloc(size);
    if (!t->bytes) { cui_mvt_tile_destroy(t); return NULL; }
    memcpy(t->bytes, bytes, size);
    t->byte_count = size;
    int layers = 0;
    /* Base geometry must claim its bounded capacity before optional names,
     * regardless of the provider's layer order in the PBF. */
    for (int pass = 0; pass < 2; pass++) {
        map_pb p = {t->bytes, t->bytes + size};
        while (p.p < p.end) {
            unsigned id, wire; map_slice layer;
            if (!map_field(&p, &id, &wire)) goto bad;
            if (id == 3 && wire == 2) {
                if ((pass == 0 && ++layers > 64) || !map_bytes(&p, &layer) ||
                    !map_layer(t, layer, pass == 1)) goto bad;
            } else if (!map_skip(&p, wire)) goto bad;
        }
    }
    if (!layers) goto bad;
    for (int i = 0; i < t->nfeatures; i++)
        if ((t->features[i].kind == 1 || t->features[i].kind == 4) &&
            !map_build_spans(t, &t->features[i])) goto bad;
    return t;
bad:
    cui_mvt_tile_destroy(t);
    return NULL;
}
size_t cui_mvt_tile_memory_usage(const cui_mvt_tile *t) {
    if (!t) return 0;
    return sizeof *t + t->byte_count + (size_t)t->cfeatures * sizeof *t->features +
           (size_t)t->cpaths * sizeof *t->paths + (size_t)t->cpoints * sizeof *t->points +
           (size_t)t->cspans * sizeof *t->spans;
}
void cui_mvt_tile_destroy(cui_mvt_tile *t) {
    if (!t) return;
    free(t->bytes); free(t->features); free(t->paths); free(t->points); free(t->spans); free(t);
}

/* Scanline spans are prepared once on the decoder worker, not recalculated
 * on every UI frame. Even-odd crossings preserve disjoint rings and holes. */
static void map_fill(cui_ctx *ui, cui_rect r, const cui_mvt_tile *t, const map_feature *f, cui_color color) {
    float scale = r.w / 256.0f;
    for (int i = 0; i < f->span_count; i++) {
        map_span s = t->spans[f->span_first + i];
        cui_draw_rect(ui, (cui_rect){r.x + s.left * scale, r.y + s.row * scale,
                                    (s.right - s.left) * scale, (s.height + 0.02f) * scale},
                      CUI_CORNERS(0), color);
    }
}
typedef struct { const cui_mvt_tile *tile; cui_texture image; float x, y, size; int z, tx, ty; } map_ref;
typedef struct {
    cui_map_view view; const cui_map_marker *markers; int nmarkers;
    cui_map_palette palette;
    int z, count, fallback_count, hovered_marker; float scale; bool dragging, defer_vector_base;
    map_ref refs[MAP_REFS], fallback[MAP_REFS];
} map_paint;
static double map_clamp(double v, double lo, double hi) { return v < lo ? lo : v > hi ? hi : v; }
static double map_world_y(double lat, int z) {
    lat = map_clamp(lat, -85.05112878, 85.05112878);
    double rad = lat * 0.017453292519943295;
    return (1.0 - log(tan(rad) + 1.0 / cos(rad)) / 3.141592653589793) * 128.0 * (1 << z);
}
static double map_lat(double y, int z) {
    double a = 3.141592653589793 * (1.0 - y / (128.0 * (1 << z)));
    return atan(sinh(a)) * 57.29577951308232;
}
static double map_world_x(double lon, int z) { return (lon + 180.0) / 360.0 * (256.0 * (1 << z)); }
static double map_lon(double x, int z) { return x / (256.0 * (1 << z)) * 360.0 - 180.0; }
static double map_wrap(double x, double size) { x = fmod(x, size); return x < 0 ? x + size : x; }
bool cui_map_world_point(int zoom, double latitude, double longitude, double *x, double *y) {
    if (!x || !y || zoom < 0 || zoom > 14 || !isfinite(latitude) || !isfinite(longitude) ||
        latitude < -90 || latitude > 90 || longitude < -180 || longitude > 180) return false;
    double width = 256.0 * (1 << zoom);
    *x = map_wrap(map_world_x(longitude, zoom), width);
    *y = map_world_y(latitude, zoom);
    return true;
}
bool cui_map_geo_from_world(int zoom, double x, double y, double *latitude, double *longitude) {
    if (!latitude || !longitude || zoom < 0 || zoom > 14 || !isfinite(x) || !isfinite(y)) return false;
    double width = 256.0 * (1 << zoom);
    *longitude = map_lon(map_wrap(x, width), zoom);
    *latitude = map_lat(map_clamp(y, 0, width), zoom);
    return true;
}
bool cui_map_project(const cui_map_projection *p, double latitude, double longitude, cui_vec2 *position) {
    if (!p || !position || p->rect.w <= 0 || p->rect.h <= 0 || !isfinite(p->scale) ||
        !isfinite(p->view.zoom) || p->scale <= 0) return false;
    int z = (int)p->view.zoom;
    double x, y;
    if (!cui_map_world_point(z, latitude, longitude, &x, &y)) return false;
    double width = 256.0 * (1 << z) * p->scale;
    double center_x = map_world_x(p->view.longitude, z) * p->scale;
    double center_y = map_world_y(p->view.latitude, z) * p->scale;
    double dx = map_wrap(x * p->scale - center_x + width / 2, width) - width / 2;
    position->x = p->rect.x + p->rect.w / 2 + (float)dx;
    position->y = p->rect.y + p->rect.h / 2 + (float)(y * p->scale - center_y);
    return position->x >= p->rect.x && position->x < p->rect.x + p->rect.w &&
           position->y >= p->rect.y && position->y < p->rect.y + p->rect.h;
}
cui_map_palette cui_map_palette_default(cui_theme_mode mode) {
    if (mode == CUI_THEME_DARK) {
        cui_map_palette p = {CUI_RGB(0x202326), CUI_RGB(0x233844), CUI_RGB(0x2C3934),
                             CUI_RGB(0x8C979E), CUI_RGB(0x566169), CUI_RGB(0xE1E4E5),
                             CUI_RGB(0x262A2D), CUI_RGB(0xF3F4F5)};
        return p;
    }
    cui_map_palette p = {CUI_RGB(0xF4F5F4), CUI_RGB(0xD7E8EE), CUI_RGB(0xE3EBE5),
                         CUI_RGB(0xB3BEC6), CUI_RGB(0xD4DBDF), CUI_RGB(0x273138),
                         CUI_RGB(0xEDF0F1), CUI_RGB(0xFFFFFF)};
    return p;
}
static void map_road(cui_ctx *ui, cui_rect r, const cui_mvt_tile *t, const map_feature *f, cui_color c, float width) {
    cui_vec2 pts[4096];
    for (int j = 0; j < f->count; j++) {
        map_path path = t->paths[f->first + j];
        if (path.count < 2 || path.count > 4096) continue;
        for (int k = 0; k < path.count; k++) {
            map_point p = t->points[path.first + k];
            pts[k] = (cui_vec2){r.x + (float)p.x * r.w / f->extent,
                                r.y + (float)p.y * r.h / f->extent};
        }
        cui_draw_polyline(ui, pts, path.count, width * (r.w / 256.0f), c, false);
    }
}
static void map_draw_base(cui_ctx *ui, const cui_mvt_tile *t, cui_rect r, int z, bool dragging,
                          const cui_map_palette *palette) {
    cui_draw_rect(ui, r, CUI_CORNERS(0), palette->land);
    if (!t) return;
    cui_push_clip(ui, r);
    for (int pass = 1; pass <= 3; pass++) for (int j = 0; j < t->nfeatures; j++) {
        const map_feature *f = &t->features[j];
        if (pass == 1 && f->kind == 4 && !dragging) map_fill(ui, r, t, f, palette->parks);
        if (pass == 2 && f->kind == 1) map_fill(ui, r, t, f, palette->water);
        if (pass == 3 && (f->kind == 2 || f->kind == 5)) {
            bool motorway = !strcmp(f->class_name, "motorway"), trunk = !strcmp(f->class_name, "trunk");
            bool primary = !strcmp(f->class_name, "primary"), secondary = !strcmp(f->class_name, "secondary");
            bool major = motorway || trunk || primary || secondary;
            if (f->kind == 5 && z < 10) continue;
            if (f->kind == 2 && ((z <= 5 && !motorway) ||
                                 (z <= 8 && !(motorway || trunk)) ||
                                 (z <= 10 && !(motorway || trunk || primary)) ||
                                 (z <= 12 && !major) || (dragging && !major))) continue;
            if (dragging && f->kind == 5) continue;
            float width = z <= 6 ? 0.75f : z <= 8 ? 0.95f :
                          motorway ? (z >= 11 ? 2.8f : 1.8f) :
                          major ? (z >= 11 ? 1.7f : 1.15f) : (z >= 12 ? 0.9f : 0.65f);
            map_road(ui, r, t, f, f->kind == 5 ? palette->water :
                     major ? palette->road_major : palette->road_minor, width);
        }
    }
    cui_pop_clip(ui);
}
void cui_map_tile_draw_base_styled(cui_ctx *ui, const cui_mvt_tile *tile, cui_rect rect,
                                   int zoom, const cui_map_palette *palette) {
    if (!ui || !tile || zoom < 0 || zoom > 20) return;
    cui_map_palette base = palette ? *palette : cui_map_palette_default(ui->mode);
    map_draw_base(ui, tile, rect, zoom, false, &base);
}
void cui_map_tile_draw_base(cui_ctx *ui, const cui_mvt_tile *tile, cui_rect rect, int zoom) {
    cui_map_tile_draw_base_styled(ui, tile, rect, zoom, NULL);
}
static float map_marker_radius(const cui_map_marker *marker) {
    return marker->count > 1 ? (marker->count >= 100 ? 14.0f : 11.0f) : 4.0f;
}
static int map_label_group(int kind, int zoom) {
    if (kind == 3) return 0; /* place */
    if (zoom < 13) return -1;
    if (kind == 7) return 1; /* transportation_name */
    if (kind == 5 || kind == 8 || kind == 9 || kind == 10) return 2; /* water and landmarks */
    if (kind == 6) return 3; /* POI */
    return -1;
}
static void map_paint_fn(cui_ctx *ui, cui_rect bounds, void *data) {
    map_paint *m = data;
    cui_draw_rect(ui, bounds, CUI_CORNERS(0), m->palette.land);
    int labels = 0, max_labels = m->z >= 13 ? MAP_LABELS : MAP_LABELS_LOW;
    cui_rect used[MAP_LABELS];
    double center_x = map_world_x(m->view.longitude, m->z) * m->scale;
    double center_y = map_world_y(m->view.latitude, m->z) * m->scale;
    double world_width = 256.0 * (1 << m->z) * m->scale;
    /* Reserve marker labels first, so place names never paint underneath them. */
    for (int i = 0; i < m->nmarkers && labels < max_labels; i++) {
        const cui_map_marker *mark = &m->markers[i];
        if (!mark->label || mark->id < 0 || !isfinite(mark->latitude) || !isfinite(mark->longitude)) continue;
        double dx = map_wrap(map_world_x(mark->longitude, m->z) * m->scale - center_x + world_width / 2, world_width) - world_width / 2;
        cui_text_metrics text = cui_measure_text(ui, CUI_FONT_BODY, 12, mark->label, -1);
        used[labels++] = (cui_rect){bounds.x + bounds.w / 2 + (float)dx +
                                      (mark->count > 1 ? map_marker_radius(mark) + 3 : 9),
                                      bounds.y + bounds.h / 2 + (float)(map_world_y(mark->latitude, m->z) * m->scale - center_y) - 7,
                                      text.width, text.height};
    }
    for (int i = 0; i < m->count; i++) {
        const cui_mvt_tile *t = m->refs[i].tile;
        cui_rect r = {bounds.x + m->refs[i].x, bounds.y + m->refs[i].y, m->refs[i].size, m->refs[i].size};
        if (!m->refs[i].image && (!t || m->defer_vector_base)) {
            bool filled = false;
            cui_push_clip(ui, bounds);
            cui_push_clip(ui, r);
            for (int j = 0; j < m->fallback_count; j++) {
                const map_ref *src = &m->fallback[j];
                cui_rect sr = {bounds.x + src->x, bounds.y + src->y, src->size, src->size};
                if (sr.x >= r.x + r.w || sr.x + sr.w <= r.x ||
                    sr.y >= r.y + r.h || sr.y + sr.h <= r.y) continue;
                if (src->image) cui_draw_image(ui, src->image, sr, (cui_rect){0, 0, 1, 1},
                                               CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
                else if (src->tile && !m->defer_vector_base) map_draw_base(ui, src->tile, sr, src->z, false, &m->palette);
                filled = true;
            }
            if (!filled) cui_draw_rect(ui, r, CUI_CORNERS(0), m->palette.placeholder);
            cui_pop_clip(ui);
            cui_pop_clip(ui);
            continue;
        }
        if (m->refs[i].image) {
            cui_push_clip(ui, r);
            cui_draw_image(ui, m->refs[i].image, r, (cui_rect){0, 0, 1, 1}, CUI_CORNERS(0), cui__rgba(255, 255, 255, 255));
            cui_pop_clip(ui);
        } else map_draw_base(ui, t, r, m->z, m->dragging, &m->palette);
    }
    /* World-anchored cell quotas and priority groups prevent dense POI layers
     * from hiding places and roads. One accepted name per group/cell at high
     * zoom, at most three groups per cell. Low zoom keeps its two place passes. */
    uint64_t cell_keys[MAP_LABEL_CELL_SLOTS] = {0};
    unsigned char cell_counts[MAP_LABEL_CELL_SLOTS] = {0};
    unsigned char cell_groups[MAP_LABEL_CELL_SLOTS] = {0};
    int groups = m->z >= 13 ? 4 : 1;
    if (bounds.w > 0 && bounds.h > 0) for (int group = 0; group < groups && labels < max_labels; group++) {
        /* Preserve room for roads, landmarks and POIs even when places are
         * very dense. Empty higher-priority groups surrender their slots. */
        int ceiling = groups == 1 ? max_labels : group == 0 ? max_labels - 128 :
                      group == 1 ? max_labels - 80 : group == 2 ? max_labels - 48 : max_labels;
        for (int pass = 0; pass < (groups == 1 ? 2 : 1) && labels < ceiling; pass++) {
        for (int i = 0; i < m->count && labels < ceiling; i++) {
            const cui_mvt_tile *t = m->refs[i].tile;
            if (!t) continue;
            float ox = bounds.x + m->refs[i].x, oy = bounds.y + m->refs[i].y;
            int waterways = 0;
            for (int j = 0; j < t->nfeatures && labels < ceiling; j++) {
                const map_feature *f = &t->features[j];
                if (!f->name[0] || !f->count || map_label_group(f->kind, m->z) != group) continue;
                if (f->kind == 5 && ++waterways > MAP_WATERWAY_CANDIDATES) continue;
                map_point p = f->anchor;
                cui_vec2 pos = {ox + (float)p.x * m->refs[i].size / f->extent,
                                oy + (float)p.y * m->refs[i].size / f->extent};
                if (pos.x < bounds.x || pos.y < bounds.y ||
                    pos.x >= bounds.x + bounds.w || pos.y >= bounds.y + bounds.h) continue;
                int col = (int)floor(((double)m->refs[i].tx * 256.0 +
                                      (double)p.x * 256.0 / f->extent) / MAP_LABEL_CELL);
                int row = (int)floor(((double)m->refs[i].ty * 256.0 +
                                      (double)p.y * 256.0 / f->extent) / MAP_LABEL_CELL);
                uint64_t key = ((uint64_t)(uint32_t)col << 32) | (uint32_t)row;
                unsigned cell = (unsigned)(key ^ (key >> 32)) * 2654435761u & (MAP_LABEL_CELL_SLOTS - 1);
                while (cell_counts[cell] && cell_keys[cell] != key)
                    cell = (cell + 1) & (MAP_LABEL_CELL_SLOTS - 1);
                if (groups == 1 ? cell_counts[cell] > pass :
                    cell_counts[cell] >= 3 || (cell_groups[cell] & (1u << group))) continue;
                cui_text_metrics text = cui_measure_text(ui, CUI_FONT_BODY, 12, f->name, -1);
                cui_rect box = {pos.x, pos.y, text.width, text.height};
                if (box.x + box.w > bounds.x + bounds.w || box.y + box.h > bounds.y + bounds.h) continue;
                bool overlap = false;
                for (int k = 0; k < labels; k++) if (box.x < used[k].x + used[k].w + 6 && box.x + box.w + 6 > used[k].x && box.y < used[k].y + used[k].h + 4 && box.y + box.h + 4 > used[k].y) { overlap = true; break; }
                if (!overlap) {
                    cell_keys[cell] = key;
                    cell_counts[cell]++;
                    cell_groups[cell] |= (unsigned char)(1u << group);
                    used[labels++] = box;
                    cui_draw_text(ui, CUI_FONT_BODY, 12, pos, m->palette.place_text, f->name, -1);
                }
            }
        }
        }
    }
    double cx = center_x, cy = center_y;
    double world = world_width;
    for (int i = 0; i < m->nmarkers; i++) {
        const cui_map_marker *mark = &m->markers[i];
        if (mark->id < 0 || !isfinite(mark->latitude) || !isfinite(mark->longitude)) continue;
        double dx = map_wrap(map_world_x(mark->longitude, m->z) * m->scale - cx + world / 2, world) - world / 2;
        cui_vec2 p = {bounds.x + bounds.w / 2 + (float)dx,
                      bounds.y + bounds.h / 2 + (float)(map_world_y(mark->latitude, m->z) * m->scale - cy)};
        float radius = map_marker_radius(mark);
        float edge = radius + 2 > 8 ? radius + 2 : 8;
        if (p.x < bounds.x - edge || p.x > bounds.x + bounds.w + edge ||
            p.y < bounds.y - edge || p.y > bounds.y + bounds.h + edge) continue;
        bool hovered = mark->id == m->hovered_marker;
        cui_color halo = hovered ? CUI_C(ui, ACCENT) : m->palette.marker_halo;
        if (hovered) halo.a = 88;
        /* Keep the hover silhouette inside the same marker hit radius. */
        cui_draw_circle(ui, p, radius + 2, halo);
        cui_draw_circle(ui, p, radius + (hovered ? 1 : 0), CUI_C(ui, ACCENT));
        if (mark->count > 1) {
            char count[16];
            if (mark->count > 999) snprintf(count, sizeof count, "999+");
            else snprintf(count, sizeof count, "%d", mark->count);
            cui_text_metrics text = cui_measure_text(ui, CUI_FONT_MEDIUM, 11, count, -1);
            cui_draw_text(ui, CUI_FONT_MEDIUM, 11,
                          (cui_vec2){p.x - text.width / 2, p.y - text.height / 2},
                          CUI_C(ui, ACCENT_FOREGROUND), count, -1);
        }
        if (mark->label) cui_draw_text(ui, CUI_FONT_BODY, 12,
                                      (cui_vec2){p.x + (mark->count > 1 ? radius + 3 : 9), p.y - 7},
                                      CUI_C(ui, FOREGROUND), mark->label, -1);
    }
}
typedef struct {
    cui_vec2 last; bool down, moved, initialized;
    int target, source, recent;
    float visual, start, elapsed;
    double anchor_lon, anchor_lat, last_lon, last_lat;
    cui_vec2 anchor;
} map_state;
static void map_zoom_to(map_state *st, int from, int to, double cx, double cy,
                        cui_vec2 pointer, cui_rect rect) {
    if (from == to) return;
    st->recent = from;
    double scale = exp2((double)st->visual - from);
    st->anchor_lon = map_lon(map_wrap((cx + pointer.x - rect.w / 2) / scale,
                                      256.0 * (1 << from)), from);
    st->anchor_lat = map_lat(map_clamp((cy + pointer.y - rect.h / 2) / scale,
                                      0, 256.0 * (1 << from)), from);
    st->anchor = pointer;
    st->start = st->visual;
    st->elapsed = 0;
    st->target = to;
}
/* Read only already-resident source tiles. Never request a non-target zoom. */
static void map_collect_fallback(map_paint *m, const cui_map_opts *opts, int z, cui_rect rect) {
    if (z < 0 || z > 20 || z == m->z) return;
    double scale = ldexp((double)m->scale, m->z - z);
    double size = 256.0 * scale;
    if (size < 32) return; /* bound high-zoom child scanning */
    double cx = map_world_x(m->view.longitude, z) * scale;
    double cy = map_world_y(m->view.latitude, z) * scale;
    int minx = (int)floor((cx - rect.w / 2) / size);
    int maxx = (int)floor((cx + rect.w / 2) / size);
    int miny = (int)floor((cy - rect.h / 2) / size);
    int maxy = (int)floor((cy + rect.h / 2) / size);
    int tiles = 1 << z, first = m->fallback_count;
    for (int y = miny; y <= maxy && m->fallback_count - first < MAP_REFS / 4; y++) {
        if (y < 0 || y >= tiles) continue;
        for (int x = minx; x <= maxx && m->fallback_count - first < MAP_REFS / 4; x++) {
            int tx = (x % tiles + tiles) % tiles;
            /* The raster-first mode never regenerates old-level vectors just
             * to cover an unbaked target tile. Other hosts keep the legacy
             * decoded-vector fallback. */
            cui_texture image = opts->raster_lookup ? opts->raster_lookup(opts->tile_user, z, tx, y) : 0;
            const cui_mvt_tile *tile = m->defer_vector_base ? NULL : opts->lookup(opts->tile_user, z, tx, y);
            if (!tile && !image) continue;
            m->fallback[m->fallback_count++] = (map_ref){tile, image,
                (float)(x * size - cx + rect.w / 2), (float)(y * size - cy + rect.h / 2), (float)size, z, tx, y};
        }
    }
}
cui_map_result cui_map_ex(cui_ctx *ui, const char *idstr, cui_map_view *view, const cui_map_opts *opts) {
    cui_map_result out = {false, -1, -1};
    if (!ui || !view) return out;
    cui_id id = cui_make_id(ui, idstr ? idstr : "map");
    int lo = opts && opts->min_zoom > 0 ? opts->min_zoom : 0;
    int hi = opts && opts->max_zoom > 0 ? opts->max_zoom : 14;
    if (hi > 20) hi = 20;
    if (lo > hi) lo = hi;
    if (!isfinite(view->longitude)) view->longitude = 0;
    if (!isfinite(view->latitude)) view->latitude = 0;
    if (!isfinite(view->zoom)) view->zoom = lo;
    int z = (int)map_clamp(floor(view->zoom + 0.5f), lo, hi);
    view->zoom = (float)z;
    view->latitude = map_clamp(view->latitude, -85.05112878, 85.05112878);
    view->longitude = map_lon(map_wrap(map_world_x(view->longitude, z), 256.0 * (1 << z)), z);
    map_paint *m = CUI_PAINT_DATA(ui, map_paint);
    memset(m, 0, sizeof *m);
    m->hovered_marker = -1;
    m->palette = opts && opts->palette ? *opts->palette : cui_map_palette_default(ui->mode);
    cui_canvas_opts canvas = {0}; canvas.focusable = true;
    cui_size w = opts && opts->width.kind ? opts->width : CUI_FILL;
    cui_size h = opts && opts->height.kind ? opts->height : CUI_FILL;
    cui_canvas_result r = cui_canvas_ex(ui, idstr ? idstr : "map", w, h, map_paint_fn, m, 0, &canvas);
    if (r.rect.w > 0 && r.rect.h > 0) cui_wheel_region(ui, id, r.rect);
    /* cui_canvas_ex may grow CUI's state array; acquire the state afterward. */
    map_state *st = cui_state(ui, id ^ 0x4D41505Au, sizeof *st);
    if (st->initialized && (fabs(view->longitude - st->last_lon) > 0.000001 ||
                            fabs(view->latitude - st->last_lat) > 0.000001))
        st->initialized = false; /* external recenter: do not animate the old area */
    if (!st->initialized || st->target != z) {
        st->initialized = true; st->target = st->source = st->recent = z;
        st->visual = st->start = (float)z; st->elapsed = 0.2f;
    }
    float motion = cui_theme_get(ui)->motion;
    if (motion <= 0) { st->visual = (float)z; st->elapsed = 0.2f; }
    else if (st->elapsed < 0.2f) {
        st->elapsed = (float)map_clamp(st->elapsed + cui_delta_time(ui) / motion, 0, 0.2);
        float t = st->elapsed / 0.2f;
        float eased = 1 - (1 - t) * (1 - t) * (1 - t);
        st->visual = st->start + (z - st->start) * eased;
        if (st->elapsed < 0.2f) cui_request_redraw(ui);
    }
    double scale = exp2((double)st->visual - z);
    double cx = map_world_x(view->longitude, z) * scale;
    double cy = map_world_y(view->latitude, z) * scale;
    if (st->elapsed < 0.2f) {
        cx = map_world_x(st->anchor_lon, z) * scale - st->anchor.x + r.rect.w / 2;
        cy = map_world_y(st->anchor_lat, z) * scale - st->anchor.y + r.rect.h / 2;
    }
    if (r.pressed) {
        if (st->down) {
            float dx = r.mouse.x - st->last.x, dy = r.mouse.y - st->last.y;
            if (dx != 0 || dy != 0) {
                cx -= dx; cy -= dy;
                st->moved = true; out.view_changed = true;
            }
        } else { st->moved = false; st->down = true; }
        st->last = r.mouse;
    } else st->down = false;
    cui_vec2 wheel;
    if (cui_wheel_take(ui, id, &wheel) && wheel.y != 0) {
        int next = (int)map_clamp(z + (wheel.y > 0 ? 1 : -1), lo, hi);
        if (next != z) {
            map_zoom_to(st, z, next, cx, cy, r.mouse, r.rect);
            z = next; scale = exp2((double)st->visual - z);
            if (motion <= 0) { st->visual = (float)z; st->elapsed = 0.2f; scale = 1; }
            cx = map_world_x(st->anchor_lon, z) * scale - st->anchor.x + r.rect.w / 2;
            cy = map_world_y(st->anchor_lat, z) * scale - st->anchor.y + r.rect.h / 2;
            out.view_changed = true; cui_request_redraw(ui);
        }
    }
    if (cui_focused_id(ui) == id) {
        if (cui_key_pressed(ui, CUI_KEY_LEFT)) { cx -= 64; out.view_changed = true; }
        if (cui_key_pressed(ui, CUI_KEY_RIGHT)) { cx += 64; out.view_changed = true; }
        if (cui_key_pressed(ui, CUI_KEY_UP)) { cy -= 64; out.view_changed = true; }
        if (cui_key_pressed(ui, CUI_KEY_DOWN)) { cy += 64; out.view_changed = true; }
        int next = z;
        if (cui_key_pressed(ui, CUI_KEY_EQUAL) && z < hi) next++;
        if (cui_key_pressed(ui, CUI_KEY_MINUS) && z > lo) next--;
        if (next != z) {
            cui_vec2 center = {r.rect.w / 2, r.rect.h / 2};
            map_zoom_to(st, z, next, cx, cy, center, r.rect);
            z = next; scale = exp2((double)st->visual - z);
            if (motion <= 0) { st->visual = (float)z; st->elapsed = 0.2f; scale = 1; }
            cx = map_world_x(st->anchor_lon, z) * scale - st->anchor.x + r.rect.w / 2;
            cy = map_world_y(st->anchor_lat, z) * scale - st->anchor.y + r.rect.h / 2;
            out.view_changed = true; cui_request_redraw(ui);
        }
    }
    double world = 256.0 * (1 << z);
    cx = map_wrap(cx, world * scale); cy = map_clamp(cy, 0, world * scale);
    view->longitude = map_lon(cx / scale, z); view->latitude = map_lat(cy / scale, z);
    view->zoom = (float)z;
    if (st->elapsed < 0.2f) {
        st->anchor_lon = map_lon(map_wrap((cx + st->anchor.x - r.rect.w / 2) / scale, world), z);
        st->anchor_lat = map_lat(map_clamp((cy + st->anchor.y - r.rect.h / 2) / scale, 0, world), z);
    }
    m->view = *view; m->z = z; m->scale = (float)scale;
    out.projection = (cui_map_projection){r.rect, *view, (float)scale, st->elapsed < 0.2f};
    m->dragging = r.pressed && st->moved;
    m->defer_vector_base = opts && opts->defer_vector_base && opts->raster_lookup;
    if (opts) { m->markers = opts->markers; m->nmarkers = opts->markers && opts->marker_count > 0 ? opts->marker_count : 0;
                if (m->nmarkers > 4096) m->nmarkers = 4096; }
    for (int i = 0; i < m->nmarkers && (r.hovered || r.clicked); i++) {
        const cui_map_marker *mark = &m->markers[i];
        if (mark->id < 0 || !isfinite(mark->latitude) || !isfinite(mark->longitude)) continue;
        double dx = map_wrap(map_world_x(mark->longitude, z) * scale - cx + world * scale / 2,
                             world * scale) - world * scale / 2;
        double x = r.rect.w / 2 + dx, y = r.rect.h / 2 + map_world_y(mark->latitude, z) * scale - cy;
        float hit_radius = mark->count > 1 ? map_marker_radius(mark) + 2 : 8;
        double mx = x - r.mouse.x, my = y - r.mouse.y;
        if (!m->dragging && mx * mx + my * my <= hit_radius * hit_radius) {
            out.hovered_marker = mark->id;
            m->hovered_marker = mark->id;
            cui__set_cursor(ui, CUI_CURSOR_HAND);
            if (r.clicked && !st->moved) out.clicked_marker = mark->id;
            break;
        }
    }
    if (r.rect.w > 0 && r.rect.h > 0 && opts && opts->lookup) {
        /* Keep geographically matching source tiles alive before target requests
         * can replace slots. Paint distant levels first so nearer cached detail
         * wins without requesting any old level. */
        if (st->source != z || st->recent != z) {
            int levels[4] = {st->source, st->recent, z - 1, z + 1};
            for (int i = 0; i < 4; i++) for (int j = i + 1; j < 4; j++)
                if (abs(levels[i] - z) < abs(levels[j] - z)) {
                    int tmp = levels[i]; levels[i] = levels[j]; levels[j] = tmp;
                }
            for (int i = 0; i < 4; i++) {
                if (levels[i] < lo || levels[i] > hi || levels[i] == z) continue;
                bool duplicate = false;
                for (int j = 0; j < i; j++) if (levels[j] == levels[i]) duplicate = true;
                if (!duplicate) map_collect_fallback(m, opts, levels[i], r.rect);
            }
        }
        int border = r.pressed ? 1 : 0;
        double size = 256.0 * scale;
        int minx = (int)floor((cx - r.rect.w / 2) / size) - border;
        int maxx = (int)floor((cx + r.rect.w / 2) / size) + border;
        int miny = (int)floor((cy - r.rect.h / 2) / size) - border;
        int maxy = (int)floor((cy + r.rect.h / 2) / size) + border;
        int tiles = 1 << z, missing = 0;
        double final_cx = st->elapsed < 0.2f ?
            map_world_x(st->anchor_lon, z) - st->anchor.x + r.rect.w / 2 : cx;
        double final_cy = st->elapsed < 0.2f ?
            map_world_y(st->anchor_lat, z) - st->anchor.y + r.rect.h / 2 : cy;
        final_cx = map_wrap(final_cx, world);
        final_cy = map_clamp(final_cy, 0, world);
        int req_minx = (int)floor((final_cx - r.rect.w / 2) / 256) - border;
        int req_maxx = (int)floor((final_cx + r.rect.w / 2) / 256) + border;
        int req_miny = (int)floor((final_cy - r.rect.h / 2) / 256) - border;
        int req_maxy = (int)floor((final_cy + r.rect.h / 2) / 256) + border;
        for (int y = miny; y <= maxy && m->count < MAP_REFS; y++) {
            if (y < 0 || y >= tiles) continue;
            for (int x = minx; x <= maxx && m->count < MAP_REFS; x++) {
                int tx = (x % tiles + tiles) % tiles;
                const cui_mvt_tile *tile = opts->lookup(opts->tile_user, z, tx, y);
                cui_texture image = opts->raster_lookup ? opts->raster_lookup(opts->tile_user, z, tx, y) : 0;
                if (!image && (!tile || m->defer_vector_base)) missing++;
                if (!tile && opts->request && x >= req_minx && x <= req_maxx &&
                    y >= req_miny && y <= req_maxy) opts->request(opts->tile_user, z, tx, y);
                m->refs[m->count++] = (map_ref){tile, image,
                    (float)(x * size - cx + r.rect.w / 2), (float)(y * size - cy + r.rect.h / 2), (float)size, z, tx, y};
            }
        }
        if (!missing) { st->source = st->recent = z; m->fallback_count = 0; }
    }
    st->last_lon = view->longitude; st->last_lat = view->latitude;
    return out;
}
