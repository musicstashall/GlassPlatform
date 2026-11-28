#pragma once
#include "GlassMaintenanceCache.h"
#include "GlassInstallLog.h"
#include "../../third_party/nlohmann/json.hpp"
#include <algorithm>
#include <array>
#include <mutex>
#include <string>
#include <vector>

// Кэш загрузок продуктов: %ProgramData%\Packages\GlassPlatform\Downloads.
// Сюда копируются пакеты, скачанные с сервера и проверенные по SHA-256 каталога,
// чтобы после отмены установки (или сбоя) не качать их заново. Кэш НИЧЕГО не
// разрешает сам по себе: обнаружение доверяет файлу только при точном совпадении
// версии с актуальной серверной и повторной проверке хеша (Discover/FindLocal).
// Манифест downloads.json хранит последний выбор продуктов и записи файлов.
// Очистка — после успешной установки и после удаления продуктов.
namespace GlassDownloadCache {

inline constexpr wchar_t kManifest[] = L"downloads.json";
inline constexpr std::size_t kSlots = 7; // слоты как у DownloadSelection (5 — SecureUxTheme)

// Прерванная установка: восстановить выбор, кроме продукта, который уже новее
// скачанного файла или новее версии, которую установка предложила бы сейчас.
inline bool RestoreInterruptedSelection(bool saved, bool installedNewerThanCache, bool installedNewerThanOffer) noexcept {
    return saved && !installedNewerThanCache && !installedNewerThanOffer;
}

struct Entry {
    unsigned slot = 0;
    std::wstring file;
    std::string version, sha256;
};
struct Manifest {
    std::array<bool, kSlots> selection{};
    bool hasSelection = false;
    std::vector<Entry> entries;
};

namespace Detail {
// Один мьютекс на процесс: UI-поток пишет выбор, рабочие потоки — файлы.
inline std::mutex& Lock() noexcept { static std::mutex lock; return lock; }
struct Handle {
    HANDLE h = INVALID_HANDLE_VALUE;
    Handle() noexcept = default;
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() noexcept { if (h != INVALID_HANDLE_VALUE) CloseHandle(h); }
};
inline std::string Narrow(const std::wstring& ascii) {
    std::string out;
    out.reserve(ascii.size());
    for (wchar_t c : ascii) out.push_back(static_cast<char>(c));
    return out;
}
inline std::wstring WideAscii(const std::string& ascii) {
    std::wstring out;
    out.reserve(ascii.size());
    for (char c : ascii) out.push_back(static_cast<wchar_t>(static_cast<unsigned char>(c)));
    return out;
}
} // namespace Detail

// Имя файла кэша: только ASCII-буквы, цифры и «._-», без ведущей точки и «..».
inline bool SafeName(const std::wstring& name) noexcept {
    if (name.empty() || name.size() > 128 || name[0] == L'.' || name.find(L"..") != std::wstring::npos) return false;
    for (wchar_t c : name) {
        const bool ok = (c >= L'0' && c <= L'9') || (c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
            c == L'.' || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    if (_wcsicmp(name.c_str(), kManifest) == 0) return false;
    return name.size() < 4 || _wcsicmp(name.c_str() + name.size() - 4, L".tmp") != 0;
}

inline std::wstring Directory() noexcept {
    try {
        const auto root = GlassMaintenanceCache::Root();
        return root.empty() ? std::wstring{} : root + L"\\Downloads";
    } catch (...) { return {}; }
}

// Путь к файлу кэша, если каталог и файл существуют и защищены (владелец BA/SY,
// без записи для не-админов, без reparse). Иначе пустая строка — обычное состояние.
inline std::wstring Path(const std::wstring& name) noexcept {
    try {
        if (!SafeName(name)) return {};
        const auto root = GlassMaintenanceCache::Root();
        const auto dir = Directory();
        if (root.empty() || dir.empty() || GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
        const auto path = dir + L"\\" + name;
        const DWORD a = GetFileAttributesW(path.c_str());
        if (a == INVALID_FILE_ATTRIBUTES || (a & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))) return {};
        if (!GlassMaintenanceCache::TrustedDirectory(root) || !GlassMaintenanceCache::TrustedDirectory(dir) ||
            !GlassMaintenanceCache::TrustedPath(path, false)) return {};
        return path;
    } catch (...) { return {}; }
}

namespace Detail {
inline bool EnsureLocked() noexcept {
    try {
        const auto dir = Directory();
        return !dir.empty() && GlassMaintenanceCache::EnsureRoot() && GlassMaintenanceCache::SecureDirectory(dir);
    } catch (...) { return false; }
}

// Вызывающий держит Lock(). Нечитаемый/чужой манифест — как отсутствующий.
inline bool ReadManifestLocked(Manifest& out) noexcept {
    out = {};
    try {
        const auto dir = Directory();
        if (dir.empty()) return false;
        const auto path = dir + L"\\" + kManifest;
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return false;
        if (!GlassMaintenanceCache::TrustedDirectory(dir) || !GlassMaintenanceCache::TrustedPath(path, false)) return false;
        std::string text;
        {
            Handle file;
            file.h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
            if (file.h == INVALID_HANDLE_VALUE) return false;
            LARGE_INTEGER size{};
            if (!GetFileSizeEx(file.h, &size) || size.QuadPart <= 0 || size.QuadPart > 64 * 1024) return false;
            text.resize(static_cast<std::size_t>(size.QuadPart));
            DWORD read = 0;
            if (!ReadFile(file.h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr) || read != text.size()) return false;
        }
        const auto json = nlohmann::json::parse(text, nullptr, false);
        if (json.is_discarded() || !json.is_object() || json.value("format", 0) != 1) return false;
        if (const auto it = json.find("selection"); it != json.end() && it->is_array() && it->size() == kSlots) {
            for (std::size_t i = 0; i < kSlots; ++i) {
                if (!(*it)[i].is_boolean()) return false;
                out.selection[i] = (*it)[i].get<bool>();
            }
            out.hasSelection = true;
        }
        if (const auto it = json.find("entries"); it != json.end()) {
            if (!it->is_array()) return false;
            for (const auto& item : *it) {
                if (!item.is_object()) return false;
                Entry entry;
                entry.slot = item.value("slot", 99u);
                entry.file = WideAscii(item.value("file", std::string{}));
                entry.version = item.value("version", std::string{});
                entry.sha256 = item.value("sha256", std::string{});
                if (entry.slot >= kSlots || !SafeName(entry.file) || entry.version.empty()) return false;
                out.entries.push_back(std::move(entry));
            }
        }
        return true;
    } catch (...) { out = {}; return false; }
}

// Вызывающий держит Lock(). Запись через .tmp + замена, файл защищается как кэш.
inline bool WriteManifestLocked(const Manifest& manifest) noexcept {
    try {
        const auto dir = Directory();
        if (dir.empty()) return false;
        nlohmann::json json = {{"format", 1}};
        if (manifest.hasSelection) json["selection"] = manifest.selection;
        auto entries = nlohmann::json::array();
        for (const auto& e : manifest.entries)
            entries.push_back({{"slot", e.slot}, {"file", Narrow(e.file)}, {"version", e.version}, {"sha256", e.sha256}});
        json["entries"] = std::move(entries);
        const std::string text = json.dump(2);
        const auto path = dir + L"\\" + kManifest;
        const auto tmp = path + L".tmp";
        {
            Handle file;
            file.h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file.h == INVALID_HANDLE_VALUE) return false;
            DWORD written = 0;
            if (!WriteFile(file.h, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
                written != text.size() || !FlushFileBuffers(file.h)) {
                CloseHandle(file.h); file.h = INVALID_HANDLE_VALUE;
                DeleteFileW(tmp.c_str());
                return false;
            }
        }
        if (!GlassMaintenanceCache::HardenFile(tmp) ||
            !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        return true;
    } catch (...) { return false; }
}
} // namespace Detail

inline bool ReadManifest(Manifest& out) noexcept {
    try {
        std::lock_guard<std::mutex> lock(Detail::Lock());
        return Detail::ReadManifestLocked(out);
    } catch (...) { out = {}; return false; }
}

// Копия проверенного пакета в кэш + запись в манифесте (замена записи того же слота).
inline bool Store(unsigned slot, const std::wstring& name, const std::wstring& source,
    const std::string& version, const std::string& sha256) noexcept {
    try {
        if (slot >= kSlots || !SafeName(name) || source.empty() || version.empty()) return false;
        std::lock_guard<std::mutex> lock(Detail::Lock());
        if (!Detail::EnsureLocked()) return false;
        const auto path = Directory() + L"\\" + name;
        const auto tmp = path + L".tmp";
        DeleteFileW(tmp.c_str());
        if (!CopyFileW(source.c_str(), tmp.c_str(), FALSE)) {
            GlassInstallLog::LineFormat(L"Download cache: copy failed source=%s error=%lu", source.c_str(), GetLastError());
            return false;
        }
        if (!GlassMaintenanceCache::HardenFile(tmp) ||
            !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            GlassInstallLog::LineFormat(L"Download cache: publish failed path=%s error=%lu", path.c_str(), GetLastError());
            DeleteFileW(tmp.c_str());
            return false;
        }
        Manifest manifest;
        Detail::ReadManifestLocked(manifest); // нечитаемый манифест заменяется новым
        manifest.entries.erase(std::remove_if(manifest.entries.begin(), manifest.entries.end(), [&](const Entry& e) {
            return e.slot == slot || _wcsicmp(e.file.c_str(), name.c_str()) == 0;
        }), manifest.entries.end());
        manifest.entries.push_back({slot, name, version, sha256});
        return Detail::WriteManifestLocked(manifest);
    } catch (...) { return false; }
}

// Последний выбор продуктов (UI-поток). Каталог создаётся при необходимости.
inline bool WriteSelection(const std::array<bool, kSlots>& selection) noexcept {
    try {
        std::lock_guard<std::mutex> lock(Detail::Lock());
        Manifest manifest;
        Detail::ReadManifestLocked(manifest);
        if (manifest.hasSelection && manifest.selection == selection) return true;
        if (!Detail::EnsureLocked()) return false;
        manifest.selection = selection;
        manifest.hasSelection = true;
        return Detail::WriteManifestLocked(manifest);
    } catch (...) { return false; }
}

// Удаление устаревшей записи и её файла (версия разошлась с серверной).
inline bool Remove(const std::wstring& name) noexcept {
    try {
        if (!SafeName(name)) return false;
        std::lock_guard<std::mutex> lock(Detail::Lock());
        const auto path = Path(name);
        bool ok = path.empty() || DeleteFileW(path.c_str()) != FALSE;
        Manifest manifest;
        if (Detail::ReadManifestLocked(manifest)) {
            const auto before = manifest.entries.size();
            manifest.entries.erase(std::remove_if(manifest.entries.begin(), manifest.entries.end(),
                [&](const Entry& e) { return _wcsicmp(e.file.c_str(), name.c_str()) == 0; }), manifest.entries.end());
            if (manifest.entries.size() != before && !Detail::WriteManifestLocked(manifest)) ok = false;
        }
        return ok;
    } catch (...) { return false; }
}

// Полная очистка кэша загрузок (плоский каталог, без вложенных/reparse).
inline bool Clear() noexcept {
    try {
        std::lock_guard<std::mutex> lock(Detail::Lock());
        const auto dir = Directory();
        return dir.empty() || GlassMaintenanceCache::RemoveFlat(dir);
    } catch (...) { return false; }
}

} // namespace GlassDownloadCache
