#ifndef CUI_MAP_TILEJSON_HPP
#define CUI_MAP_TILEJSON_HPP
#include <string>

/* Provider-specific input validation for the Windows demo, not CUI core. */
struct map_tilejson {
    std::string path_template;
    int min_zoom;
    int max_zoom;
};
bool map_parse_tilejson(const std::string &text, map_tilejson &out, std::string &error);
#endif
