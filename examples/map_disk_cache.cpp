#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>
#include "map_disk_cache.hpp"
#include <atomic>
#include <algorithm>
#include <functional>
#include <cwctype>
#include <cstring>

namespace {
const size_t tile_limit = 2u * 1024u * 1024u;
const size_t json_limit = 65536;
std::atomic<unsigned long> temp_sequence(0);

bool directory(const std::wstring &path) {
    if (CreateDirectoryW(path.c_str(), NULL)) return true;
    if (GetLastError() != ERROR_ALREADY_EXISTS) return false;
    DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}
bool directory_tree(const std::wstring &path) {
    size_t start = path.compare(0, 4, L"\\\\?\\") == 0 ? 7 :
                   path.size() >= 3 && path[1] == L':' ? 3 : 0;
    if (!start) return false; /* only absolute local paths are accepted */
    for (size_t i = start; i < path.size(); ++i)
        if (path[i] == L'\\' && !directory(path.substr(0, i))) return false;
    return directory(path);
}
uint64_t file_time(const FILETIME &time) {
    ULARGE_INTEGER value = {};
    value.LowPart = time.dwLowDateTime; value.HighPart = time.dwHighDateTime;
    return value.QuadPart;
}
bool decimal_name(const wchar_t *name) {
    if (!*name) return false;
    for (; *name; ++name) if (*name < L'0' || *name > L'9') return false;
    return true;
}
bool hash_name(const wchar_t *name) {
    size_t n = 0;
    for (; name[n]; ++n) {
        if (n == 64 || !((name[n] >= L'0' && name[n] <= L'9') ||
                          (name[n] >= L'a' && name[n] <= L'f'))) return false;
    }
    return n == 64;
}
bool tile_name(const wchar_t *name) {
    size_t n = wcslen(name);
    if (n <= 4 || wcscmp(name + n - 4, L".pbf")) return false;
    for (size_t i = 0; i < n - 4; ++i)
        if (name[i] < L'0' || name[i] > L'9') return false;
    return true;
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
map_disk_cache::map_disk_cache(const std::wstring &root, uint64_t budget_bytes)
    : override_root_(root), ready_(false), budget_(budget_bytes) {}
void map_disk_cache::set_budget(uint64_t budget_bytes) { budget_.store(budget_bytes); }

void map_disk_cache::index_tiles() {
    if (indexed_ || !init()) return;
    entries_.clear(); used_bytes_ = 0;
    std::function<void(const std::wstring &, int)> scan = [&](const std::wstring &dir, int depth) {
        WIN32_FIND_DATAW data = {};
        HANDLE search = FindFirstFileW((dir + L"\\*").c_str(), &data);
        if (search == INVALID_HANDLE_VALUE) return;
        do {
            if (!wcscmp(data.cFileName, L".") || !wcscmp(data.cFileName, L"..") ||
                (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
            std::wstring path = dir + L"\\" + data.cFileName;
            if (depth < 3) {
                bool valid = depth == 0 ? hash_name(data.cFileName) : decimal_name(data.cFileName);
                if (valid && (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) scan(path, depth + 1);
            } else if (!(data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && tile_name(data.cFileName)) {
                uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
                if (UINT64_MAX - used_bytes_ < size) { used_bytes_ = UINT64_MAX; continue; }
                used_bytes_ += size;
                entries_.emplace(path, entry{size, file_time(data.ftLastWriteTime)});
            }
        } while (FindNextFileW(search, &data));
        FindClose(search);
    };
    scan(root_, 0);
    indexed_ = true;
}
void map_disk_cache::note_removed(const std::wstring &path) {
    if (!indexed_) return;
    auto it = entries_.find(path);
    if (it == entries_.end()) return;
    used_bytes_ -= it->second.size;
    entries_.erase(it);
}
void map_disk_cache::note_tile(const std::wstring &path) {
    if (!indexed_) return;
    note_removed(path);
    WIN32_FILE_ATTRIBUTE_DATA data = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data) ||
        (data.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return;
    uint64_t size = (uint64_t(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    used_bytes_ = UINT64_MAX - used_bytes_ < size ? UINT64_MAX : used_bytes_ + size;
    entries_.emplace(path, entry{size, file_time(data.ftLastWriteTime)});
}
void map_disk_cache::trim_locked() {
    uint64_t budget = budget_.load();
    if (!budget) return;
    index_tiles();
    if (used_bytes_ <= budget) return;
    std::vector<std::wstring> oldest;
    oldest.reserve(entries_.size());
    for (const auto &item : entries_) oldest.push_back(item.first);
    std::sort(oldest.begin(), oldest.end(), [this](const std::wstring &a, const std::wstring &b) {
        uint64_t a_time = entries_.at(a).used, b_time = entries_.at(b).used;
        return a_time == b_time ? a < b : a_time < b_time;
    });
    for (const std::wstring &path : oldest) {
        if (used_bytes_ <= budget) break;
        if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
            note_removed(path);
    }
}
void map_disk_cache::trim() {
    if (!budget_.load()) return;
    std::lock_guard<std::mutex> lock(files_lock_);
    trim_locked();
}

bool map_disk_cache::init() {
    std::call_once(init_, [this]() {
        if (!override_root_.empty()) {
            root_ = override_root_;
            ready_ = budget_.load() ? directory_tree(root_) : directory(root_);
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
    if (create) {
        if (!directory(src) || !directory(level) || !directory(column)) return false;
    } else {
        for (const std::wstring *dir : {&src, &level, &column}) {
            DWORD attrs = GetFileAttributesW(dir->c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY) ||
                (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
        }
    }
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
    if (!tile_path(templ, z, x, y, path, false)) return false;
    std::lock_guard<std::mutex> lock(files_lock_);
    const DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        if (attrs == INVALID_FILE_ATTRIBUTES) note_removed(path);
        return false;
    }
    bool ok = read_file(path, tile_limit, bytes);
    if (!ok && indexed_) note_tile(path); /* read_file may have removed an invalid entry */
    return ok;
}
bool map_disk_cache::write_tile(const char *templ, int z, int x, int y, const void *bytes, size_t length) {
    std::wstring path;
    uint64_t budget = budget_.load();
    if (length > tile_limit || (budget && length > budget) ||
        !tile_path(templ, z, x, y, path, true)) return false;
    std::lock_guard<std::mutex> lock(files_lock_);
    if (budget_.load()) index_tiles();
    if (!write_file(path, bytes, length)) return false;
    note_tile(path);
    if (budget_.load() && used_bytes_ > budget_.load()) trim_locked();
    if (budget_.load() && used_bytes_ > budget_.load()) {
        if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND)
            note_removed(path);
        return false; /* a locked/undeletable older tile blocked the quota */
    }
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
void map_disk_cache::remove_tile(const char *templ, int z, int x, int y) {
    std::wstring path;
    if (!tile_path(templ, z, x, y, path, false)) return;
    std::lock_guard<std::mutex> lock(files_lock_);
    if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) note_removed(path);
}
void map_disk_cache::touch_tile(const char *templ, int z, int x, int y) {
    std::wstring path;
    if (!tile_path(templ, z, x, y, path, false)) return;
    std::lock_guard<std::mutex> lock(files_lock_);
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) {
        if (attrs == INVALID_FILE_ATTRIBUTES) note_removed(path);
        return;
    }
    HANDLE f = CreateFileW(path.c_str(), FILE_WRITE_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return;
    FILETIME now; GetSystemTimeAsFileTime(&now);
    if (SetFileTime(f, NULL, NULL, &now)) {
        if (indexed_) {
            auto it = entries_.find(path);
            if (it != entries_.end()) it->second.used = file_time(now);
        }
    }
    CloseHandle(f);
}
bool map_disk_cache::read_tilejson(std::string &bytes) {
    std::wstring path;
    if (!json_path(path, false)) return false;
    std::lock_guard<std::mutex> lock(files_lock_);
    return read_file(path, json_limit, bytes);
}
bool map_disk_cache::write_tilejson(const void *bytes, size_t length) {
    std::wstring path;
    if (length > json_limit || !json_path(path, true)) return false;
    std::lock_guard<std::mutex> lock(files_lock_);
    return write_file(path, bytes, length);
}
