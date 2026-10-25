// unins000.exe: читает unins000.dat (PRODUCT_CODE={GUID}) и запускает удаление MSI.
// Сначала копирует себя во временный каталог, чтобы MSI мог удалить файлы установки.
#include <windows.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <string>

namespace {

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

std::wstring ReadProductCode(const std::wstring& datPath) {
    HANDLE file = CreateFileW(datPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return L"";
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart <= 0 || size.QuadPart > 4096) {
        CloseHandle(file);
        return L"";
    }
    std::string bytes(static_cast<size_t>(size.QuadPart), '\0');
    DWORD read = 0;
    const BOOL ok = ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr);
    CloseHandle(file);
    if (!ok || read == 0) return L"";
    const std::string prefix = "PRODUCT_CODE=";
    const auto pos = bytes.find(prefix);
    if (pos == std::string::npos) return L"";
    std::string guid = bytes.substr(pos + prefix.size());
    while (!guid.empty() && (guid.back() == '\r' || guid.back() == '\n' || guid.back() == ' '))
        guid.pop_back();
    if (guid.empty()) return L"";
    if (guid.front() != '{') guid = "{" + guid;
    if (guid.back() != '}') guid.push_back('}');
    int count = MultiByteToWideChar(CP_UTF8, 0, guid.c_str(), static_cast<int>(guid.size()), nullptr, 0);
    if (count <= 0) return L"";
    std::wstring wide(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, guid.c_str(), static_cast<int>(guid.size()), wide.data(), count);
    return wide;
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

void Fail(const wchar_t* text) {
    LogLine(text);
    const LANGID lang = PRIMARYLANGID(GetUserDefaultUILanguage());
    const wchar_t* title = lang == LANG_RUSSIAN ? L"Удаление GlassPlatform" : L"GlassPlatform uninstall";
    MessageBoxW(nullptr, text, title, MB_OK | MB_ICONERROR);
}

int UninstallProduct() noexcept {
    wchar_t exeDir[MAX_PATH]{};
    if (!GetModuleFileNameW(nullptr, exeDir, MAX_PATH)) return 1;
    PathRemoveFileSpecW(exeDir);
    wchar_t datPath[MAX_PATH]{};
    if (!PathCombineW(datPath, exeDir, L"unins000.dat")) return 2;
    const std::wstring productCode = ReadProductCode(datPath);
    if (productCode.empty()) {
        Fail(L"unins000.dat не содержит PRODUCT_CODE.");
        return 3;
    }
    LogLine(L"msiexec /x " + productCode);
    const std::wstring args = L"/x " + productCode + L" /qn /norestart";
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    info.lpVerb = L"open";
    info.lpFile = L"msiexec.exe";
    info.lpParameters = args.c_str();
    info.nShow = SW_HIDE;
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        Fail(L"Не удалось запустить msiexec.");
        return 4;
    }
    WaitForSingleObject(info.hProcess, INFINITE);
    DWORD exitCode = 1;
    GetExitCodeProcess(info.hProcess, &exitCode);
    CloseHandle(info.hProcess);
    LogLine(L"msiexec exit " + std::to_wstring(exitCode));
    if (exitCode != 0 && exitCode != 3010 && exitCode != 1641) {
        wchar_t message[160]{};
        swprintf_s(message, L"Удаление не выполнено. Код MSI: %lu", exitCode);
        Fail(message);
        return static_cast<int>(exitCode);
    }
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
    const std::wstring params = L"/fromtemp";
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
    if (!HasSwitch(L"/fromtemp")) return RelaunchFromTemp();
    return UninstallProduct();
}
