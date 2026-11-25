#include "map_tilejson.hpp"
#include "vendor/json11/json11.hpp"
#include <cmath>

namespace {
bool zoom_field(const json11::Json &root, const char *key, int &value) {
    const json11::Json &v = root[key];
    double n = v.number_value();
    if (!v.is_number() || !std::isfinite(n) || std::floor(n) != n || n < 0 || n > 14) return false;
    value = static_cast<int>(n);
    return true;
}
}

bool map_parse_tilejson(const std::string &text, map_tilejson &out, std::string &error) {
    error.clear();
    if (text.empty() || text.size() > 65536) { error = "TileJSON size is invalid"; return false; }
    std::string parse_error;
    json11::Json root = json11::Json::parse(text, parse_error, json11::JsonParse::STANDARD);
    if (!parse_error.empty() || !root.is_object()) { error = "Invalid TileJSON object"; return false; }
    const json11::Json &tiles = root["tiles"];
    if (!tiles.is_array() || tiles.array_items().empty() || !tiles[0].is_string()) {
        error = "TileJSON has no vector tile URL"; return false;
    }
    int min_zoom, max_zoom;
    if (!zoom_field(root, "minzoom", min_zoom) || !zoom_field(root, "maxzoom", max_zoom) || min_zoom > max_zoom) {
        error = "TileJSON zoom range is unsupported"; return false;
    }
    const std::string &url = tiles[0].string_value();
    const std::string origin = "https://tiles.openfreemap.org";
    const std::string prefix = origin + "/planet/";
    const std::string suffix = "/{z}/{x}/{y}.pbf";
    if (url.compare(0, prefix.size(), prefix) != 0 || url.size() > origin.size() + 239) {
        error = "Tile URL host or length is unsupported"; return false;
    }
    const std::string path = url.substr(origin.size());
    const size_t at = path.find(suffix);
    const std::string version = at == std::string::npos ? std::string() : path.substr(8, at - 8);
    if (at == std::string::npos || version.empty() || version == "." || version == ".." ||
        version.find('/') != std::string::npos || version.find('{') != std::string::npos ||
        version.find('}') != std::string::npos || at + suffix.size() != path.size() ||
        path.find("{z}") != at + 1 || path.find("{x}") != at + 5 || path.find("{y}") != at + 9) {
        error = "Tile URL coordinate template is unsupported"; return false;
    }
    for (char c : path) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' ||
              c == '.' || c == '{' || c == '}')) {
            error = "Tile URL path contains an unsupported character"; return false;
        }
    }
    map_tilejson parsed = {path, min_zoom, max_zoom};
    out = parsed;
    return true;
}
