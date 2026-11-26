#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include "map_disk_cache.hpp"
#include <atomic>
#include <cstring>

namespace {
const size_t tile_limit = 2u * 1024u * 1024u;
const size_t json_limit = 65536;
std::atomic<unsigned long> temp_sequence(0);

bool directory(const std::wstring &path) {
    if (CreateDirectoryW(path.c_str(), NULL)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
}
bool valid_template(const char *value) {
    if (!value) return false;
    std::string path(value);
    const std::string prefix = "/planet/", suffix = "/{z}/{x}/{y}.pbf";
    if (path.size() > 239 || path.compare(0, prefix.size(), prefix) != 0 ||
        path.size() <= prefix.size() + suffix.size() ||
        path.compare(path.size() - suffix.size(), suffix.size(), suffix) != 0) return false;
    std::string version = path.substr(prefix.size(), path.size() - prefix.size() - suffix.size());
    if (version == "." || version == "..") return false;
    for (size_t i = 0; i < version.size(); i++) {
        char c = version[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}
bool source_hash(const char *path, std::wstring &hex) {
    const std::string identity = std::string("https://tiles.openfreemap.org") + path;
    BCRYPT_ALG_HANDLE alg = NULL;
    if (!BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, NULL, 0))) return false;
    unsigned char digest[32] = {};
    NTSTATUS result = BCryptHash(alg, NULL, 0,
                                (PUCHAR)identity.data(), (ULONG)identity.size(), digest, sizeof digest);
    BCryptCloseAlgorithmProvider(alg, 0);
    if (!BCRYPT_SUCCESS(result)) return false;
    static const wchar_t digits[] = L"0123456789abcdef";
    hex.clear(); hex.reserve(64);
    for (size_t i = 0; i < sizeof digest; i++) {
        hex += digits[digest[i] >> 4]; hex += digits[digest[i] & 15];
    }
    return true;
}
}

map_disk_cache::map_disk_cache() : ready_(false) {}
map_disk_cache::map_disk_cache(const std::wstring &root) : override_root_(root), ready_(false) {}

bool map_disk_cache::init() {
    std::call_once(init_, [this]() {
        if (!override_root_.empty()) {
            root_ = override_root_;
            ready_ = directory(root_);
            return;
        }
        PWSTR base = NULL;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, NULL, &base)) || !base) return;
        std::wstring local = L"\\\\?\\" + std::wstring(base);
        CoTaskMemFree(base);
        ready_ = directory(local + L"\\CUI") &&
                 directory(local + L"\\CUI\\MapTiles") &&
                 directory(local + L"\\CUI\\MapTiles\\v1");
        if (ready_) root_ = local + L"\\CUI\\MapTiles\\v1";
    });
    return ready_;
}

bool map_disk_cache::tile_path(const char *tile_template, int z, int x, int y,
                               std::wstring &path, bool create) {
    if (!valid_template(tile_template) || z < 0 || z > 14 || x < 0 || y < 0 ||
        x >= (1 << z) || y >= (1 << z) || !init()) return false;
    std::wstring hash;
    if (!source_hash(tile_template, hash)) return false;
    std::wstring src = root_ + L"\\" + hash;
    std::wstring level = src + L"\\" + std::to_wstring(z);
    std::wstring column = level + L"\\" + std::to_wstring(x);
    if (create && (!directory(src) || !directory(level) || !directory(column))) return false;
    path = column + L"\\" + std::to_wstring(y) + L".pbf";
    return true;
}
bool map_disk_cache::json_path(std::wstring &path, bool create) {
    if (!init()) return false;
    (void)create;
    path = root_ + L"\\last-tilejson.json";
    return true;
}
bool map_disk_cache::read_file(const std::wstring &path, size_t cap, std::string &bytes) {
    bytes.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER length = {};
    bool sized = GetFileSizeEx(f, &length) != 0;
    bool invalid_size = sized && (length.QuadPart <= 0 ||
                                   (unsigned long long)length.QuadPart > cap);
    bool ok = sized && !invalid_size;
    if (ok) {
        bytes.resize((size_t)length.QuadPart);
        size_t n = 0;
        while (n < bytes.size()) {
            DWORD got = 0;
            if (!ReadFile(f, &bytes[n], (DWORD)(bytes.size() - n), &got, NULL) || !got) { ok = false; break; }
            n += got;
        }
    }
    CloseHandle(f);
    if (invalid_size) DeleteFileW(path.c_str()); /* corrupt or oversized entry */
    if (!ok) bytes.clear();
    return ok;
}
bool map_disk_cache::write_file(const std::wstring &path, const void *bytes, size_t length) {
    if (!bytes || !length || length > tile_limit) return false;
    std::wstring tmp = path + L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
                       std::to_wstring(GetTickCount64()) + L"." +
                       std::to_wstring(++temp_sequence) + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, NULL, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(f, bytes, (DWORD)length, &written, NULL) && written == length && FlushFileBuffers(f);
    CloseHandle(f);
    if (ok) ok = MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(tmp.c_str());
    return ok;
}
bool map_disk_cache::read_tile(const char *templ, int z, int x, int y, std::string &bytes) {
    std::wstring path;
    return tile_path(templ, z, x, y, path, false) && read_file(path, tile_limit, bytes);
}
bool map_disk_cache::write_tile(const char *templ, int z, int x, int y, const void *bytes, size_t length) {
    std::wstring path;
    return length <= tile_limit && tile_path(templ, z, x, y, path, true) && write_file(path, bytes, length);
}
void map_disk_cache::remove_tile(const char *templ, int z, int x, int y) {
    std::wstring path;
    if (tile_path(templ, z, x, y, path, false)) DeleteFileW(path.c_str());
}
void map_disk_cache::touch_tile(const char *templ, int z, int x, int y) {
    std::wstring path;
    if (!tile_path(templ, z, x, y, path, false)) return;
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    SetFileTime(f, NULL, NULL, &now);
    CloseHandle(f);
}
bool map_disk_cache::read_tilejson(std::string &bytes) {
    std::wstring path;
    return json_path(path, false) && read_file(path, json_limit, bytes);
}
bool map_disk_cache::write_tilejson(const void *bytes, size_t length) {
    std::wstring path;
    return length <= json_limit && json_path(path, true) && write_file(path, bytes, length);
}
