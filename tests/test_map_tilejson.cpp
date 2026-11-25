#include "../examples/map_tilejson.hpp"
#include <cstdio>
#include <string>

static int failures;
static void check(const std::string &json, bool expected, const char *why) {
    map_tilejson parsed = {"unchanged", 7, 8}; std::string error;
    bool ok = map_parse_tilejson(json, parsed, error);
    if (ok != expected || (!ok && error.empty()) ||
        (!ok && (parsed.path_template != "unchanged" || parsed.min_zoom != 7 || parsed.max_zoom != 8))) {
        std::fprintf(stderr, "TileJSON test %s: got %d (%s), expected %d\n", why, ok, error.c_str(), expected);
        failures++;
    }
    if (ok && (parsed.path_template != "/planet/version_1/{z}/{x}/{y}.pbf" || parsed.min_zoom != 0 || parsed.max_zoom != 14)) {
        std::fprintf(stderr, "TileJSON test %s: decoded wrong template/zoom\n", why); failures++;
    }
}
int main() {
    const char *valid = "https://tiles.openfreemap.org/planet/version_1/{z}/{x}/{y}.pbf";
    const std::string good = std::string("{\"tiles\":[\"") + valid + "\"],\"minzoom\":0,\"maxzoom\":14}";
    check(good, true, "valid");
    check(std::string("{\"extra\":{\"tiles\":[\"malicious\"]},\"maxzoom\":14,\"minzoom\":0,\"tiles\":[\"") + valid + "\"],\"name\":\"\\\"escaped\\\"\"}", true, "reordered and escaped unrelated fields");
    check("{", false, "syntax");
    check("[]", false, "non-object");
    check("{\"tiles\":[] ,\"minzoom\":0,\"maxzoom\":14}", false, "empty array");
    check("{\"tiles\":[4],\"minzoom\":0,\"maxzoom\":14}", false, "wrong tile type");
    check(std::string("{\"tiles\":[\"") + valid + "\"],\"minzoom\":\"0\",\"maxzoom\":14}", false, "wrong zoom type");
    check(std::string("{\"tiles\":[\"") + valid + "\"],\"minzoom\":14,\"maxzoom\":0}", false, "reversed zoom");
    check(std::string("{\"tiles\":[\"") + valid + "\"],\"minzoom\":0.5,\"maxzoom\":14}", false, "fractional zoom");
    check(std::string("{\"tiles\":[\"") + valid + "\"],\"minzoom\":0,\"maxzoom\":15}", false, "unsupported zoom");
    check("{\"tiles\":[\"https://tiles.openfreemap.org.evil.test/planet/version_1/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "host confusion");
    check("{\"tiles\":[\"http://tiles.openfreemap.org/planet/version_1/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "not HTTPS");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/private/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "wrong path");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/planet/version_1/{z}/{x}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "duplicate coordinate");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/planet/version_1/{z}/{x}/{y}.pbf?token=x\"],\"minzoom\":0,\"maxzoom\":14}", false, "query");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/planet/../{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "path traversal");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/planet/extra/sub/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "extra path segment");
    check("{\"tiles\":[\"https://tiles.openfreemap.org/planet/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}", false, "unversioned");
    check(std::string(65537, ' '), false, "size limit");
    std::fprintf(stderr, "TileJSON tests: %s\n", failures ? "FAILED" : "OK");
    return failures ? 1 : 0;
}
