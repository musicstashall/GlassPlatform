#pragma once

#include <string>
#include <vector>

// Traversal policy shared by the Win32 adapter and portable tests. Only
// ordinary directories are visited; files and reparse points are never removed.
namespace GlassEmptyDirectories {

struct Entry {
    std::wstring name;
    bool directory = false;
    bool reparse = false;
};

inline bool SafeName(const std::wstring& name) noexcept {
    return !name.empty() && name != L"." && name != L".." &&
        name.find_first_of(L"\\/") == std::wstring::npos;
}

template<class FileSystem>
void Prune(FileSystem& fs, const std::wstring& path, unsigned depth = 0) {
    // Bound stack use even if an install path contains a pathological tree.
    if (depth >= 128) return;
    auto directory = fs.OpenOrdinaryDirectory(path);
    if (!directory) return;
    std::vector<Entry> entries;
    if (!fs.ReadEntries(directory, path, entries)) return;
    for (const auto& entry : entries) {
        if (!entry.directory || entry.reparse || !SafeName(entry.name)) continue;
        Prune(fs, path + L"\\" + entry.name, depth + 1);
    }
    // The adapter removes by the still-open handle, and only if still empty.
    fs.RemoveIfEmpty(directory, path);
}

} // namespace GlassEmptyDirectories
