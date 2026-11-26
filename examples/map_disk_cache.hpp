#ifndef CUI_MAP_DISK_CACHE_HPP
#define CUI_MAP_DISK_CACHE_HPP
#include <string>
#include <mutex>

/* Windows demo only: uncompressed, validated-by-caller tile bytes. No CUI
 * context, HTTP, JSON dependency, or automatic eviction. All calls on workers. */
class map_disk_cache {
public:
    map_disk_cache();
    explicit map_disk_cache(const std::wstring &root); /* isolated test root */
    bool read_tile(const char *tile_template, int z, int x, int y, std::string &bytes);
    bool write_tile(const char *tile_template, int z, int x, int y, const void *bytes, size_t length);
    void remove_tile(const char *tile_template, int z, int x, int y);
    void touch_tile(const char *tile_template, int z, int x, int y);
    bool read_tilejson(std::string &bytes);
    bool write_tilejson(const void *bytes, size_t length);
private:
    std::once_flag init_;
    std::wstring root_, override_root_;
    bool ready_;
    bool init();
    bool tile_path(const char *tile_template, int z, int x, int y,
                   std::wstring &path, bool create);
    bool json_path(std::wstring &path, bool create);
    static bool read_file(const std::wstring &path, size_t cap, std::string &bytes);
    static bool write_file(const std::wstring &path, const void *bytes, size_t length);
};
#endif
