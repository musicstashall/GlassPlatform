// unins000.exe: стоп хоста, watch, удаление platform MSI затем GlassSettings MSI.
#include "InstallSupport/GlassInstallHost.h"
#include "InstallSupport/GlassInstallMsi.h"
#include "InstallSupport/GlassInstallUnlock.h"
#include "InstallSupport/GlassInstallUtil.h"
#include "InstallSupport/GlassInstallWatch.h"

#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <commctrl.h>
#include <msi.h>
#include <string>
#include <vector>

#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {

constexpr wchar_t kPlatformX64[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5071}";
constexpr wchar_t kPlatformX86[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5072}";
constexpr wchar_t kOpenGlass[] = L"{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5073}";

void LogLine(const std::wstring& line) {
    wchar_t temp[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, temp)) return;
    const std::wstring path = std::wstring(temp) + L"GlassUnins.log";
    HANDLE file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr,
        OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    const std::wstring text = line + L"\r\n";
    DWORD written = 0;
    WriteFile(file, text.data(), static_cast<DWORD>(text.size() * sizeof(wchar_t)), &written, nullptr);
    CloseHandle(file);
}

std::wstring ReadIniValueUtf8(const std::string& bytes, const char* key) {
    const std::string prefix = std::string(key) + "=";
    size_t pos = 0;
    for (;;) {
        pos = bytes.find(prefix, pos);
        if (pos == std::string::npos) return L"";
        if (pos == 0 || bytes[pos - 1] == '\n' || bytes[pos - 1] == '\r')
            break;
        pos += prefix.size();
    }
    std::string value = bytes.substr(pos + prefix.size());
    const auto eol = value.find_first_of("\r\n");
    if (eol != std::string::npos) value.resize(eol);
    while (!value.empty() && value.back() == ' ')
        value.pop_back();
    if (value.empty()) return L"";
    if (value.front() != '{') value = "{" + value;
    if (value.back() != '}') value.push_back('}');
    int count = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.c_str(), static_cast<int>(value.size()), wide.data(), count);
    return wide;
}

bool ReadDatFile(const std::wstring& datPath, std::wstring& productCode, std::wstring& settingsCode) {
    HANDLE file = CreateFileW(datPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 8192) {
        CloseHandle(file);
        return false;
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return false;
    bytes.resize(read);
    productCode = ReadIniValueUtf8(bytes, "PRODUCT_CODE");
    settingsCode = ReadIniValueUtf8(bytes, "SETTINGS_PRODUCT_CODE");
    return !productCode.empty();
}

bool HasSwitch(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return false;
    bool found = false;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0) { found = true; break; }
    }
    LocalFree(argv);
    return found;
}

std::wstring ArgAfterSwitch(const wchar_t* name) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return L"";
    std::wstring value;
    for (int i = 1; i < argc; ++i) {
        if (_wcsicmp(argv[i], name) == 0 && i + 1 < argc)
            value = argv[i + 1];
    }
    LocalFree(argv);
    return value;
}

std::wstring RegInstallPath(bool x64) {
    HKEY key = nullptr;
    const REGSAM sam = KEY_READ | (x64 ? KEY_WOW64_64KEY : KEY_WOW64_32KEY);
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\GlassPlatform", 0, sam, &key) != ERROR_SUCCESS)
        return L"";
    wchar_t buf[MAX_PATH * 2]{};
    DWORD cb = sizeof(buf);
    DWORD type = 0;
    const LSTATUS st = RegQueryValueExW(key, L"InstallPath", nullptr, &type, reinterpret_cast<LPBYTE>(buf), &cb);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS || type != REG_SZ) return L"";
    std::wstring path = buf;
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/'))
        path.pop_back();
    return path;
}

void Fail(const wchar_t* text) {
    LogLine(text);
    const LANGID lang = PRIMARYLANGID(GetUserDefaultUILanguage());
    const wchar_t* title = lang == LANG_RUSSIAN ? L"Удаление GlassPlatform" : L"GlassPlatform uninstall";
    TaskDialog(nullptr, nullptr, title, nullptr, text, TDCBF_OK_BUTTON, TD_INFORMATION_ICON, nullptr);
}

enum class UninstallChoice { Retry, Unlock, Cancel };

UninstallChoice AskUser(const std::wstring& report) {
    const bool ru = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN;
    TASKDIALOGCONFIG cfg{};
    cfg.cbSize = sizeof(cfg);
    cfg.dwFlags = TDF_ALLOW_DIALOG_CANCELLATION | TDF_USE_COMMAND_LINKS;
    cfg.pszWindowTitle = ru ? L"Удаление GlassPlatform" : L"GlassPlatform uninstall";
    cfg.pszMainInstruction = ru
        ? L"Процессы всё ещё используют модули Glass"
        : L"Processes still hold Glass modules";
    cfg.pszContent = report.c_str();
    const TASKDIALOG_BUTTON buttons[] = {
        { static_cast<int>(UninstallChoice::Retry), ru ? L"Повторить (принудительная выгрузка)" : L"Retry (force unload)" },
        { static_cast<int>(UninstallChoice::Unlock), ru ? L"Закрыть процессы" : L"Close processes" },
        { static_cast<int>(UninstallChoice::Cancel), ru ? L"Отмена" : L"Cancel" },
    };
    cfg.pButtons = buttons;
    cfg.cButtons = static_cast<UINT>(std::size(buttons));
    cfg.nDefaultButton = static_cast<int>(UninstallChoice::Retry);
    int pressed = 0;
    TaskDialogIndirect(&cfg, &pressed, nullptr, nullptr);
    if (pressed == static_cast<int>(UninstallChoice::Cancel) || pressed == 0)
        return UninstallChoice::Cancel;
    if (pressed == static_cast<int>(UninstallChoice::Unlock))
        return UninstallChoice::Unlock;
    return UninstallChoice::Retry;
}

bool EnsureModulesReleased(
    const std::wstring& x64Dir,
    const std::wstring& x86Dir,
    bool scanNativeX64) {
    for (;;) {
        bool nativeFailed = false;
        std::vector<GlassInstallUnlock::ModuleHolder> holders =
            GlassInstallUnlock::CollectHolders(x64Dir, x86Dir, scanNativeX64, nativeFailed);
        if (holders.empty()) return true;
        if (GlassInstallWatch::WaitUntilClearOrTimeout(30000, x64Dir, x86Dir, scanNativeX64, holders, nativeFailed)
            && holders.empty())
            return true;
        holders = GlassInstallUnlock::CollectHolders(x64Dir, x86Dir, scanNativeX64, nativeFailed);
        if (holders.empty()) return true;
        const auto choice = AskUser(GlassInstallUnlock::FormatHolders(holders));
        if (choice == UninstallChoice::Cancel)
            return false;
        if (choice == UninstallChoice::Retry) {
            for (const auto& h : holders)
                (void)GlassInstallUnlock::ForceUnloadHolder(h, x64Dir, x86Dir);
            continue;
        }
        for (const auto& h : holders)
            GlassInstallUnlock::UnlockProcess(h.pid, h.exe.c_str());
    }
}

UINT RemoveProduct(const wchar_t* productCode) {
    if (!productCode || !productCode[0]) return ERROR_SUCCESS;
    MsiSetInternalUI(INSTALLUILEVEL_NONE, nullptr);
    LogLine(L"MsiConfigureProductEx ABSENT " + std::wstring(productCode));
    return MsiConfigureProductExW(productCode, INSTALLLEVEL_DEFAULT, INSTALLSTATE_ABSENT,
        L"REBOOT=ReallySuppress");
}

int RunUninstallFromDir(const std::wstring& installDir) {
    wchar_t datPath[MAX_PATH]{};
    if (!PathCombineW(datPath, installDir.c_str(), L"unins000.dat")) return 2;
    std::wstring productCode;
    std::wstring settingsCode;
    if (!ReadDatFile(datPath, productCode, settingsCode)) {
        Fail(L"unins000.dat не содержит PRODUCT_CODE.");
        return 3;
    }

    const bool isOpenGlass = _wcsicmp(productCode.c_str(), kOpenGlass) == 0;
    const bool isX64 = _wcsicmp(productCode.c_str(), kPlatformX64) == 0;
    const bool isX86 = _wcsicmp(productCode.c_str(), kPlatformX86) == 0;
    if (!isOpenGlass && !isX64 && !isX86) {
        Fail(L"Неизвестный PRODUCT_CODE в unins000.dat.");
        return 4;
    }

    std::wstring x64Dir = RegInstallPath(true);
    std::wstring x86Dir = RegInstallPath(false);
    if (x64Dir.empty()) x64Dir = installDir;
    if (x86Dir.empty()) x86Dir = installDir;

    if (!GlassInstallHost::StopAllForUninstall(x64Dir, x86Dir, isX64, isX86, isOpenGlass)) {
        LogLine(L"Host stop timed out — continuing with module watch");
    }

    if (!isOpenGlass) {
        const bool scanNative = isX64;
        if (!EnsureModulesReleased(x64Dir, x86Dir, scanNative))
            return 10;
    }

    UINT code = RemoveProduct(productCode.c_str());
    if (code != ERROR_SUCCESS && code != ERROR_SUCCESS_REBOOT_INITIATED && code != ERROR_SUCCESS_REBOOT_REQUIRED) {
        wchar_t message[160]{};
        swprintf_s(message, L"Удаление platform MSI не выполнено. Код: %u", code);
        Fail(message);
        return static_cast<int>(code);
    }

    if (!settingsCode.empty() && !isOpenGlass) {
        code = RemoveProduct(settingsCode.c_str());
        if (code != ERROR_SUCCESS && code != ERROR_SUCCESS_REBOOT_INITIATED && code != ERROR_SUCCESS_REBOOT_REQUIRED) {
            wchar_t message[160]{};
            swprintf_s(message, L"Удаление GlassSettings MSI не выполнено. Код: %u", code);
            Fail(message);
            return static_cast<int>(code);
        }
    }

    if (isOpenGlass)
        GlassInstallHost::RemoveOpenGlassServiceAndTasks();
    else
        GlassInstallHost::RemovePlatformAutorunTasks(isX64, isX86);

    LogLine(L"Uninstall completed");
    return 0;
}

int RelaunchFromTemp() noexcept {
    wchar_t exePath[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exePath, MAX_PATH)) return 1;
    wchar_t exeDir[MAX_PATH]{};
    wcscpy_s(exeDir, exePath);
    PathRemoveFileSpecW(exeDir);
    wchar_t datPath[MAX_PATH]{};
    if (!PathCombineW(datPath, exeDir, L"unins000.dat")) return 2;

    wchar_t tempRoot[MAX_PATH]{};
    if (!GetTempPathW(MAX_PATH, tempRoot)) return 5;
    wchar_t destDir[MAX_PATH]{};
    swprintf_s(destDir, L"%sGlassUnins-%u", tempRoot, GetTickCount());
    if (!CreateDirectoryW(destDir, nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return 5;
    wchar_t destExe[MAX_PATH]{};
    wchar_t destDat[MAX_PATH]{};
    if (!PathCombineW(destExe, destDir, L"unins000.exe")) return 5;
    if (!PathCombineW(destDat, destDir, L"unins000.dat")) return 5;
    if (!CopyFileW(exePath, destExe, FALSE) || !CopyFileW(datPath, destDat, FALSE)) {
        Fail(L"Не удалось скопировать программу удаления во временный каталог.");
        return 6;
    }
    const std::wstring params = L"/fromtemp \"" + std::wstring(exeDir) + L"\"";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    info.lpFile = destExe;
    info.lpParameters = params.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info)) {
        Fail(L"Не удалось запустить программу удаления из временного каталога.");
        return 7;
    }
    return 0;
}

} // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    INITCOMMONCONTROLSEX icc{ sizeof(icc), ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);
    if (!HasSwitch(L"/fromtemp")) return RelaunchFromTemp();
    std::wstring installDir = ArgAfterSwitch(L"/fromtemp");
    if (installDir.empty()) {
        wchar_t exeDir[MAX_PATH]{};
        if (GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) {
            PathRemoveFileSpecW(exeDir);
            installDir = exeDir;
        }
    }
    if (installDir.empty()) {
        Fail(L"Не задан каталог установки.");
        return 8;
    }
    return RunUninstallFromDir(installDir);
}
