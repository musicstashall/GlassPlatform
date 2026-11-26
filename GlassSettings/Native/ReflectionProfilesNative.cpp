#include <Windows.h>
#include <ShlObj.h>
#include <bcrypt.h>
#include <ReflectionProfileApi.h>
#include <DebugLog.h>
#include <algorithm>
#include <string>
#include <vector>
#include <cstdio>
#include <cwchar>
#ifndef _WIN64
#pragma comment(linker, "/EXPORT:GlassSettings_CaptureReflectionProfileV1=_GlassSettings_CaptureReflectionProfileV1@4")
#pragma comment(linker, "/EXPORT:GlassSettings_SignalReflectionDisplayChangeV1=_GlassSettings_SignalReflectionDisplayChangeV1@0")
#pragma comment(linker, "/EXPORT:GlassSettings_TickReflectionProfilesV1=_GlassSettings_TickReflectionProfilesV1@0")
#endif
#pragma comment(lib, "bcrypt.lib")

namespace
{
    using namespace Shared::ReflectionProfiles;
    ULONGLONG due = 0;
    std::wstring lastKey;
    struct Key
    {
        HKEY value = nullptr;
        ~Key() { if (value) RegCloseKey(value); }
    };
    std::wstring ReadString(HKEY key, const wchar_t* name)
    {
        DWORD bytes = 0, type = 0;
        if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &bytes) || type != REG_SZ || bytes > 65536) return {};
        std::vector<wchar_t> text(bytes / sizeof(wchar_t) + 1, 0);
        if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(text.data()), &bytes)) return {};
        return text.data();
    }
    void WriteString(HKEY key, const wchar_t* name, const std::wstring& value)
    {
        const auto error = RegSetValueExW(key, name, 0, REG_SZ,
            reinterpret_cast<const BYTE*>(value.c_str()), static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
        if (error) throw error;
    }
    void WriteDword(HKEY key, const wchar_t* name, DWORD value)
    {
        const auto error = RegSetValueExW(key, name, 0, REG_DWORD,
            reinterpret_cast<const BYTE*>(&value), sizeof(value));
        if (error) throw error;
    }
    bool SafeFile(const std::wstring& path)
    {
        auto current = path;
        while (!current.empty())
        {
            const DWORD attributes = GetFileAttributesW(current.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
            auto slash = current.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash <= 2) break;
            current.resize(slash);
        }
        return true;
    }
    // Directory GlassSettings publishes canvases into; also proves earlier ownership.
    std::wstring PublishedDirectory()
    {
        std::wstring directory;
        PWSTR programData = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_ProgramData, 0, nullptr, &programData)))
        {
            directory = std::wstring(programData) + L"\\OpenGlass\\Presets\\Textured";
            CoTaskMemFree(programData);
        }
        return directory;
    }
    bool InsidePublishedDirectory(const std::wstring& path)
    {
        const auto directory = PublishedDirectory();
        if (directory.empty() || path.size() <= directory.size() + 1) return false;
        if (_wcsnicmp(path.c_str(), directory.c_str(), directory.size()) != 0) return false;
        if (path[directory.size()] != L'\\') return false;
        const auto name = path.substr(directory.size() + 1);
        return name.find_first_of(L"\\/:") == std::wstring::npos && name.find(L"..") == std::wstring::npos;
    }
    bool ValidHash(const std::wstring& path, const std::wstring& expected, LONG width, LONG height)
    {
        if (expected.size() != 64 || !SafeFile(path)) return false;
        HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0 || length.QuadPart > 67108864) { CloseHandle(file); return false; }
        // Check actual PNG signature/IHDR and canvas dimensions, not only its filename.
        BYTE header[24]{}; DWORD headerRead = 0;
        const BYTE signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
        bool validHeader = ReadFile(file, header, sizeof(header), &headerRead, nullptr) && headerRead == sizeof(header);
        for (int i = 0; i < 8; ++i) validHeader = validHeader && header[i] == signature[i];
        auto integer = [&](int offset) -> DWORD { return (DWORD(header[offset]) << 24) | (DWORD(header[offset + 1]) << 16)
            | (DWORD(header[offset + 2]) << 8) | header[offset + 3]; };
        validHeader = validHeader && integer(8) == 13 && header[12] == 'I' && header[13] == 'H'
            && header[14] == 'D' && header[15] == 'R' && integer(16) == DWORD(width) && integer(20) == DWORD(height);
        if (!validHeader || SetFilePointer(file, 0, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER)
        { CloseHandle(file); return false; }
        BCRYPT_ALG_HANDLE algorithm = nullptr;
        BCRYPT_HASH_HANDLE hash = nullptr;
        bool ok = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0;
        if (ok) ok = BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
        BYTE buffer[65536], digest[32]; DWORD read = 0;
        while (ok)
        {
            if (!ReadFile(file, buffer, sizeof(buffer), &read, nullptr)) { ok = false; break; }
            if (!read) break;
            ok = BCryptHashData(hash, buffer, read, 0) >= 0;
        }
        if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        if (!ok) return false;
        std::wstring hex;
        for (BYTE value : digest) { hex += L"0123456789ABCDEF"[value >> 4]; hex += L"0123456789ABCDEF"[value & 15]; }
        return _wcsicmp(hex.c_str(), expected.c_str()) == 0;
    }
    struct Enumeration { Snapshot* snapshot; HRESULT error = S_OK; };
    BOOL CALLBACK Enumerate(HMONITOR monitor, HDC, LPRECT rect, LPARAM data)
    {
        auto& state = *reinterpret_cast<Enumeration*>(data);
        auto& snapshot = *state.snapshot;
        if (snapshot.count >= 32) { state.error = HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW); return FALSE; }
        using DpiFn = HRESULT(WINAPI*)(HMONITOR, int, UINT*, UINT*);
        HMODULE shcore = LoadLibraryExW(L"shcore.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto dpi = shcore ? reinterpret_cast<DpiFn>(GetProcAddress(shcore, "GetDpiForMonitor")) : nullptr;
        UINT x = 96, y = 96;
        if (dpi) state.error = dpi(monitor, 0, &x, &y);
        else state.error = HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        if (shcore) FreeLibrary(shcore);
        if (FAILED(state.error)) return FALSE;
        snapshot.monitors[snapshot.count++] = {rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, x, y};
        return TRUE;
    }
    HRESULT CaptureSnapshot(Snapshot* snapshot)
    {
        if (!snapshot || snapshot->size != sizeof(Snapshot)) return E_INVALIDARG;
        *snapshot = {}; snapshot->size = sizeof(Snapshot); snapshot->version = Version;
        auto previous = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        if (!previous) return HRESULT_FROM_WIN32(GetLastError());
        Enumeration state{snapshot};
        const BOOL ok = EnumDisplayMonitors(nullptr, nullptr, Enumerate, reinterpret_cast<LPARAM>(&state));
        if (previous) SetThreadDpiAwarenessContext(previous);
        if (!ok || FAILED(state.error) || !snapshot->count) return FAILED(state.error) ? state.error : E_FAIL;
        auto begin = snapshot->monitors;
        std::sort(begin, begin + snapshot->count, [](const Monitor& a, const Monitor& b)
        {
            if (a.x != b.x) return a.x < b.x;
            if (a.y != b.y) return a.y < b.y;
            if (a.width != b.width) return a.width < b.width;
            if (a.height != b.height) return a.height < b.height;
            if (a.dpiX != b.dpiX) return a.dpiX < b.dpiX;
            return a.dpiY < b.dpiY;
        });
        LONG left = begin[0].x, top = begin[0].y, right = left, bottom = top;
        for (UINT i = 0; i < snapshot->count; i++)
        {
            auto& m = begin[i]; left = (std::min)(left, m.x); top = (std::min)(top, m.y);
            right = (std::max)(right, m.x + m.width); bottom = (std::max)(bottom, m.y + m.height);
        }
        snapshot->width = right - left; snapshot->height = bottom - top;
        // FNV-1a over canonical explicit integers (not struct padding or device IDs).
        ULONGLONG fingerprint = 14695981039346656037ull;
        for (UINT i = 0; i < snapshot->count; i++)
        {
            auto m = begin[i]; m.x -= left; m.y -= top;
            const DWORD values[] = {static_cast<DWORD>(m.x), static_cast<DWORD>(m.y), static_cast<DWORD>(m.width), static_cast<DWORD>(m.height), m.dpiX, m.dpiY};
            for (DWORD value : values) for (int byte = 0; byte < 4; byte++) { fingerprint ^= (value >> (8 * byte)) & 255; fingerprint *= 1099511628211ull; }
        }
        swprintf_s(snapshot->key, L"%ldx%ld_DPI%u_%016llX", snapshot->width, snapshot->height, begin[0].dpiX, fingerprint);
        return S_OK;
    }
    HRESULT Synchronize()
    {
        Snapshot current{}; current.size = sizeof(current);
        HRESULT result = CaptureSnapshot(&current);
        if (FAILED(result)) return result;
        DebugLog(DBG_IMPORTANT, L"[GlassSettings][ReflectionProfiles] reconcile profile=%ls canvas=%ldx%ld monitors=%lu",
            current.key, current.width, current.height, current.count);
        if (lastKey == current.key) return S_FALSE;
        HANDLE mutex = CreateMutexW(nullptr, FALSE, MutexName);
        if (!mutex) return HRESULT_FROM_WIN32(GetLastError());
        DWORD wait = WaitForSingleObject(mutex, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) { CloseHandle(mutex); return HRESULT_FROM_WIN32(ERROR_BUSY); }
        try
        {
            Key root, dwm;
            if (RegOpenKeyExW(HKEY_CURRENT_USER, RegistryRoot, 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &root.value))
            {
                DebugLog(DBG_NORMAL, L"[GlassSettings][ReflectionProfiles] skip: no HKCU profile state (no legacy adoption)");
                lastKey = current.key;
                ReleaseMutex(mutex); CloseHandle(mutex); return S_FALSE;
            }
            if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\DWM", 0, KEY_READ | KEY_WRITE | KEY_WOW64_64KEY, &dwm.value)) throw static_cast<DWORD>(ERROR_NOT_FOUND);
            auto owned = ReadString(root.value, L"ManagedOutput");
            auto active = ReadString(dwm.value, L"CustomThemeReflection");
            DWORD managed = 0, bytes = sizeof(managed);
            RegGetValueW(root.value, nullptr, L"Managed", RRF_RT_REG_DWORD, nullptr, &managed, &bytes);
            // Same ABI decision table as GlassSettings: never a second rule set.
            const auto published = PublishedDirectory();
            std::wstring output;
            Key profile;
            auto subkey = std::wstring(L"Profiles\\") + current.key;
            if (!RegOpenKeyExW(root.value, subkey.c_str(), 0, KEY_READ, &profile.value))
            {
                DWORD ready = 0; bytes = sizeof(ready);
                RegGetValueW(profile.value, nullptr, L"Ready", RRF_RT_REG_DWORD, nullptr, &ready, &bytes);
                auto candidate = ReadString(profile.value, L"Output");
                auto name = candidate.size() > published.size() + 1 ? candidate.substr(published.size() + 1) : L"";
                if (ready && InsidePublishedDirectory(candidate)
                    && name.find(current.key) != std::wstring::npos
                    && ValidHash(candidate, ReadString(profile.value, L"OutputHash"), current.width, current.height)) output = candidate;
            }
            const auto decision = Decide(managed != 0, active.empty(), InsidePublishedDirectory(active), !output.empty());
            if (decision.action == Action::None)
            {
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ReflectionProfiles] skip unmanaged: Managed=%lu active=%ls owned=%ls ready=%d",
                    managed, active.c_str(), owned.c_str(), output.empty() ? 0 : 1);
                lastKey = current.key; result = S_FALSE;
            }
            else
            {
                DebugLog(DBG_IMPORTANT, L"[GlassSettings][ReflectionProfiles] selection=%ls action=%d active=%ls output=%ls",
                    decision.action == Action::Clear ? L"clear stale profile canvas" : L"verified cached profile",
                    static_cast<int>(decision.action), active.c_str(), output.c_str());
                Snapshot check{}; check.size = sizeof(check);
                if (FAILED(CaptureSnapshot(&check)) || wcscmp(current.key, check.key)) throw static_cast<DWORD>(ERROR_RETRY);
                if (decision.action == Action::Clear)
                {
                    auto error = RegDeleteValueW(dwm.value, L"CustomThemeReflection");
                    if (error && error != ERROR_FILE_NOT_FOUND) throw static_cast<DWORD>(error);
                    // Same end state as GS ClearStaleProfileCanvas: nothing is ours now.
                    WriteString(root.value, L"ManagedOutput", L"");
                    WriteDword(root.value, L"Managed", 0);
                    HWND window = FindWindowW(L"Dwm", nullptr);
                    const BOOL posted = window ? PostMessageW(window, WM_THEMECHANGED, 0, 0) : FALSE;
                    DebugLog(posted ? DBG_IMPORTANT : DBG_WARNING,
                        L"[GlassSettings][ReflectionProfiles] cleared; Dwm=%p WM_THEMECHANGED posted=%d",
                        window, posted);
                }
                else
                {
                    // Adopt: the ready canvas of the current profile becomes applied.
                    if (_wcsicmp(active.c_str(), output.c_str()))
                    {
                        WriteString(dwm.value, L"CustomThemeReflection", output);
                        // Match GS reflection reload: address the Dwm window, never restart OG.
                        HWND window = FindWindowW(L"Dwm", nullptr);
                        const BOOL posted = window ? PostMessageW(window, WM_THEMECHANGED, 0, 0) : FALSE;
                        DebugLog(posted ? DBG_IMPORTANT : DBG_WARNING,
                            L"[GlassSettings][ReflectionProfiles] applied; Dwm=%p WM_THEMECHANGED posted=%d error=%lu",
                            window, posted, posted ? ERROR_SUCCESS : (window ? GetLastError() : ERROR_NOT_FOUND));
                    }
                    // Ownership after the value, as GS AdoptReadyProfile does: the next
                    // Read() resolves the profile the applied value now points at.
                    WriteString(root.value, L"ManagedOutput", output);
                    WriteDword(root.value, L"Managed", 1);
                }
                lastKey = current.key; result = S_OK;
            }
        }
        catch (DWORD error) { result = HRESULT_FROM_WIN32(error); }
        catch (...) { result = E_FAIL; }
        ReleaseMutex(mutex); CloseHandle(mutex);
        return result;
    }
}
extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_CaptureReflectionProfileV1(Shared::ReflectionProfiles::Snapshot* snapshot) noexcept
{
    try { return CaptureSnapshot(snapshot); } catch (...) { return E_FAIL; }
}
extern "C" __declspec(dllexport) void WINAPI GlassSettings_SignalReflectionDisplayChangeV1() noexcept
{
    lastKey.clear();
    due = GetTickCount64() + 750;
}
extern "C" __declspec(dllexport) HRESULT WINAPI GlassSettings_TickReflectionProfilesV1() noexcept
{
    if (!due || GetTickCount64() < due) return S_FALSE;
    due = 0;
    HRESULT result = Synchronize();
    if (FAILED(result))
    {
        DebugLog(DBG_WARNING, L"[GlassSettings][ReflectionProfiles] reconcile failed hr=0x%08lX; retry scheduled", result);
        due = GetTickCount64() + 2000;
    }
    return result;
}
