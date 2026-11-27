#ifndef CUI_MAP_DISK_CACHE_HPP
#define CUI_MAP_DISK_CACHE_HPP
#include <string>
#include <mutex>
#include <atomic>
#include <vector>
#include <unordered_map>
#include <cstdint>

/* Windows demo only: uncompressed, validated-by-caller tile bytes. No CUI
 * context, HTTP, or JSON dependency. A zero budget preserves the demo's
 * unbounded legacy behavior; a nonzero budget enables worker-side LRU. */
class map_disk_cache {
public:
    map_disk_cache();
    explicit map_disk_cache(const std::wstring &root); /* isolated test root */
    map_disk_cache(const std::wstring &root, uint64_t budget_bytes); /* bounded host */
    void set_budget(uint64_t budget_bytes); /* nonblocking; call trim() on a worker */
    void trim(); /* never call from the UI thread */
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
    struct entry { uint64_t size; uint64_t used; };
    std::mutex files_lock_;
    std::unordered_map<std::wstring, entry> entries_;
    uint64_t used_bytes_ = 0;
    bool indexed_ = false;
    std::atomic<uint64_t> budget_{0};
    void index_tiles(); /* files_lock_ held */
    void trim_locked(); /* files_lock_ held */
    void note_removed(const std::wstring &path); /* files_lock_ held */
    void note_tile(const std::wstring &path); /* files_lock_ held */
    bool init();
    bool tile_path(const char *tile_template, int z, int x, int y,
                   std::wstring &path, bool create);
    bool json_path(std::wstring &path, bool create);
    static bool read_file(const std::wstring &path, size_t cap, std::string &bytes);
    static bool write_file(const std::wstring &path, const void *bytes, size_t length);
};
#endif
