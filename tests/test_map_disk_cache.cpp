#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cui/cui.h>
#include "../examples/map_disk_cache.hpp"
#include "../examples/map_fixture.h"
#include "../examples/map_tilejson.hpp"
#include <cstdio>
#include <cstring>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(expr) do { if (!(expr)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #expr); failures++; } } while (0)

static void clear_directory(const std::wstring &path) {
    WIN32_FIND_DATAW data = {};
    HANDLE search = FindFirstFileW((path + L"\\*").c_str(), &data);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..")) continue;
            std::wstring file = path + L"\\" + data.cFileName;
            if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) clear_directory(file);
            else DeleteFileW(file.c_str());
        } while (FindNextFileW(search, &data));
        FindClose(search);
    }
    RemoveDirectoryW(path.c_str());
}
int main(void) {
    wchar_t temp[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, temp);
    if (!n || n >= MAX_PATH) return 2;
    std::wstring root = std::wstring(temp) + L"cui-disk-test-" +
                        std::to_wstring(GetCurrentProcessId()) + L"-" +
                        std::to_wstring(GetTickCount64());
    const char *source_a = "/planet/202611/{z}/{x}/{y}.pbf";
    const char *source_b = "/planet/202612/{z}/{x}/{y}.pbf";
    map_fixture_buf raw = map_fixture();
    {
        map_disk_cache disk(root);
        std::string bytes;
        CHECK(!disk.read_tile(source_a, 7, 12, 19, bytes));
        CHECK(disk.write_tile(source_a, 7, 12, 19, raw.b, raw.n));
        CHECK(disk.read_tile(source_a, 7, 12, 19, bytes));
        CHECK(bytes == std::string((char *)raw.b, raw.n));
        CHECK(!disk.read_tile(source_b, 7, 12, 19, bytes));
        CHECK(!disk.read_tile(source_a, 7, 128, 19, bytes));
        CHECK(!disk.write_tile("/planet/../{z}/{x}/{y}.pbf", 7, 12, 19, raw.b, raw.n));
        disk.touch_tile(source_a, 7, 12, 19);
        const char *valid = "{\"tiles\":[\"https://tiles.openfreemap.org/planet/202611/{z}/{x}/{y}.pbf\"],\"minzoom\":0,\"maxzoom\":14}";
        map_tilejson parsed; std::string error;
        CHECK(map_parse_tilejson(valid, parsed, error));
        CHECK(disk.write_tilejson(valid, strlen(valid)));
        CHECK(disk.read_tilejson(bytes));
        CHECK(map_parse_tilejson(bytes, parsed, error));
        std::atomic<int> write_failures(0);
        std::vector<std::thread> writers;
        for (int i = 0; i < 4; i++) writers.emplace_back([&]() {
            if (!disk.write_tile(source_a, 7, 13, 19, raw.b, raw.n)) ++write_failures;
        });
        for (size_t i = 0; i < writers.size(); i++) writers[i].join();
        CHECK(write_failures.load() == 0);
        CHECK(disk.read_tile(source_a, 7, 13, 19, bytes));
        CHECK(bytes == std::string((char *)raw.b, raw.n));
    }
    {
        map_disk_cache restarted(root);
        std::string bytes;
        CHECK(restarted.read_tile(source_a, 7, 12, 19, bytes));
        cui_mvt_tile *tile = cui_mvt_tile_decode(bytes.data(), bytes.size());
        CHECK(tile != NULL); cui_mvt_tile_destroy(tile);
        CHECK(restarted.write_tile(source_a, 7, 12, 19, "bad", 3));
        CHECK(restarted.read_tile(source_a, 7, 12, 19, bytes));
        tile = cui_mvt_tile_decode(bytes.data(), bytes.size());
        CHECK(tile == NULL); cui_mvt_tile_destroy(tile);
        restarted.remove_tile(source_a, 7, 12, 19);
        CHECK(!restarted.read_tile(source_a, 7, 12, 19, bytes));
        std::string oversized(2u * 1024u * 1024u + 1, 'x');
        CHECK(!restarted.write_tile(source_a, 7, 12, 19, oversized.data(), oversized.size()));
    }
    clear_directory(root);
    return failures ? 1 : 0;
}
