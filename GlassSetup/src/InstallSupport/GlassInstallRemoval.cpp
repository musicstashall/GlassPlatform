#include "GlassInstallRemoval.h"
#include "GlassInstallLog.h"
#include "GlassInstallUtil.h"
#include "GlassRemovalOrder.h"

#include <GlassPlatformTask.h>
#include <aclapi.h>
#include <objbase.h>
#include <oleauto.h>
#include <shellapi.h>
#include <shlobj.h>
#include <taskschd.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include <cwchar>
#include <iterator>
#include <utility>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using GlassInstallRemoval::Session;
using GlassInstallRemoval::TaskSnapshot;
namespace GP = GlassShared::GlassPlatformTask;

constexpr LONG kTaskSecurity = OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION |
    DACL_SECURITY_INFORMATION | SACL_SECURITY_INFORMATION;
// For filesystem objects, request all descriptor parts including labels and
// resource attributes, not just the filtered audit SACL.
constexpr SECURITY_INFORMATION kFileSecurity = kTaskSecurity | BACKUP_SECURITY_INFORMATION;

struct Bstr {
    BSTR value = nullptr;
    Bstr() noexcept = default;
    explicit Bstr(const std::wstring& s) noexcept : value(SysAllocStringLen(s.data(), static_cast<UINT>(s.size()))) {}
    ~Bstr() noexcept { if (value) SysFreeString(value); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR* out() noexcept { return &value; }
};

struct ComGuard {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ~ComGuard() noexcept { if (SUCCEEDED(result)) CoUninitialize(); }
    bool ok() const noexcept { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};

// Reading and restoring a task SACL requires SeSecurityPrivilege. Do not omit it
// silently: deleting a customised task without its complete ACL is unsafe.
struct ScopedPrivilege {
    HANDLE token = nullptr;
    TOKEN_PRIVILEGES previous{};
    bool enabled = false;
    explicit ScopedPrivilege(const wchar_t* name) noexcept {
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) return;
        LUID luid{};
        if (!LookupPrivilegeValueW(nullptr, name, &luid)) return;
        TOKEN_PRIVILEGES requested{};
        requested.PrivilegeCount = 1;
        requested.Privileges[0].Luid = luid;
        requested.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
        DWORD size = sizeof(previous);
        SetLastError(ERROR_SUCCESS);
        enabled = AdjustTokenPrivileges(token, FALSE, &requested, sizeof(previous), &previous, &size)
            && GetLastError() == ERROR_SUCCESS;
    }
    ~ScopedPrivilege() noexcept {
        if (token) {
            if (enabled) (void)AdjustTokenPrivileges(token, FALSE, &previous, 0, nullptr, nullptr);
            CloseHandle(token);
        }
    }
};

std::wstring Problem(bool ru, const wchar_t* r, const wchar_t* e, const std::wstring& item = {}, unsigned code = 0) {
    std::wstring text = ru ? r : e;
    if (!item.empty()) text += L"\r\n" + item;
    if (code) text += L"\r\n" + std::wstring(ru ? L"Код: " : L"Code: ") + std::to_wstring(code);
    return text;
}

bool Cancelled(const std::atomic_bool* cancel) noexcept {
    return cancel && cancel->load(std::memory_order_acquire);
}

bool Connect(ComPtr<ITaskService>& service) noexcept {
    const HRESULT hr = CoCreateInstance(CLSID_TaskScheduler, nullptr, CLSCTX_INPROC_SERVER,
        IID_ITaskService, reinterpret_cast<void**>(service.GetAddressOf()));
    if (FAILED(hr) || !service) return false;
    VARIANT empty{}; VariantInit(&empty);
    return SUCCEEDED(service->Connect(empty, empty, empty, empty));
}

bool GetFolder(ITaskService* service, const std::wstring& path, ComPtr<ITaskFolder>& folder) noexcept {
    Bstr name(path);
    return name.value && SUCCEEDED(service->GetFolder(name.value, folder.GetAddressOf())) && folder;
}

std::wstring ExpandAndNormalize(const std::wstring& input) {
    if (input.empty()) return {};
    std::wstring value = input;
    if (value.size() >= 2 && value.front() == L'"' && value.back() == L'"')
        value = value.substr(1, value.size() - 2);
    const DWORD need = ExpandEnvironmentStringsW(value.c_str(), nullptr, 0);
    if (!need || need > 32768) return {};
    std::vector<wchar_t> expanded(need);
    if (!ExpandEnvironmentStringsW(value.c_str(), expanded.data(), need)) return {};
    if (wcschr(expanded.data(), L'%')) return {}; // unresolved environment variable
    const std::wstring absolute(expanded.data());
    if (absolute.size()<3 || absolute[1]!=L':' || (absolute[2]!=L'\\' && absolute[2]!=L'/'))
        return {}; // Task Scheduler may resolve a relative path in a different working directory.
    const DWORD fullNeed = GetFullPathNameW(expanded.data(), 0, nullptr, nullptr);
    if (!fullNeed || fullNeed > 32768) return {};
    std::vector<wchar_t> full(fullNeed + 1);
    if (!GetFullPathNameW(expanded.data(), static_cast<DWORD>(full.size()), full.data(), nullptr)) return {};
    return GlassInstallUtil::TrimTrailingSlashes(full.data());
}

bool SamePath(const std::wstring& a, const std::wstring& b) {
    const auto left = ExpandAndNormalize(a), right = ExpandAndNormalize(b);
    return !left.empty() && !right.empty() && _wcsicmp(left.c_str(), right.c_str()) == 0;
}

bool ExpectedPlatformArguments(const std::wstring& arguments, const std::wstring& expected) {
    const std::wstring command = L"placeholder.exe " + arguments;
    int count = 0;
    LPWSTR* argv = CommandLineToArgvW(command.c_str(), &count);
    bool matches=false;
    if (argv && count==3 && _wcsicmp(argv[2],L"/start")==0) {
        std::wstring module=argv[1];
        const auto comma=module.rfind(L',');
        if (comma!=std::wstring::npos && _wcsicmp(module.c_str()+comma+1,L"Main")==0) {
            module.resize(comma);
            matches=SamePath(module,expected);
        }
    }
    if (argv) LocalFree(argv);
    return matches;
}

bool ExpectedAction(ITaskDefinition* definition, bool platform, const std::wstring& expected,
    GP::PlatformArch arch = GP::PlatformArch::X86) noexcept {
    ComPtr<IActionCollection> actions;
    LONG count = 0;
    if (FAILED(definition->get_Actions(actions.GetAddressOf())) || !actions ||
        FAILED(actions->get_Count(&count)) || count != 1) return false;
    ComPtr<IAction> action;
    TASK_ACTION_TYPE type{};
    if (FAILED(actions->get_Item(1, action.GetAddressOf())) || !action ||
        FAILED(action->get_Type(&type)) || type != TASK_ACTION_EXEC) return false;
    ComPtr<IExecAction> exec;
    if (FAILED(action->QueryInterface(IID_IExecAction, reinterpret_cast<void**>(exec.GetAddressOf()))) || !exec) return false;
    Bstr path, arguments;
    if (FAILED(exec->get_Path(path.out())) || !path.value || FAILED(exec->get_Arguments(arguments.out()))) return false;
    const std::wstring args=arguments.value ? arguments.value : L"";
    if (!platform) return args.find_first_not_of(L" \t\r\n")==std::wstring::npos && SamePath(path.value,expected);
    return SamePath(path.value,GlassInstallUtil::RundllPathForTaskArch(arch)) && ExpectedPlatformArguments(args,expected);
}

bool WriteTextBackup(const std::wstring& path, const std::wstring& text) noexcept {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const WORD bom = 0xfeff;
    DWORD written = 0;
    bool ok = WriteFile(file, &bom, sizeof(bom), &written, nullptr) && written == sizeof(bom);
    const size_t bytes = text.size() * sizeof(wchar_t);
    if (bytes > MAXDWORD) ok = false;
    if (ok && bytes) ok = WriteFile(file, text.data(), static_cast<DWORD>(bytes), &written, nullptr) && written == bytes;
    CloseHandle(file);
    return ok;
}

bool CaptureTask(IRegisteredTask* task, bool platform, const std::wstring& expected,
    Session& session, const std::wstring& workspace, bool ru, std::wstring& error) noexcept {
    Bstr path, xml, security;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    ComPtr<ITaskDefinition> definition;
    ComPtr<IPrincipal> principal;
    TASK_LOGON_TYPE logon = TASK_LOGON_NONE;
    if (FAILED(task->get_Path(path.out())) || !path.value ||
        FAILED(task->get_Definition(definition.GetAddressOf())) || !definition ||
        FAILED(definition->get_Principal(principal.GetAddressOf())) || !principal ||
        FAILED(principal->get_LogonType(&logon))) {
        error = Problem(ru, L"Не удалось проверить настройки задачи автозапуска.", L"Could not inspect the startup task.");
        return false;
    }
    // Even S4U can require a password to register a task for another account.
    // Without those credentials a faithful recreation is not guaranteed.
    if (logon == TASK_LOGON_PASSWORD || logon == TASK_LOGON_INTERACTIVE_TOKEN_OR_PASSWORD || logon == TASK_LOGON_S4U) {
        error = Problem(ru,
            L"Учётную запись этой задачи нельзя надёжно восстановить без учётных данных. Установка остановлена до изменений.",
            L"This task's account cannot be reliably restored without credentials. Setup stopped before making changes.", path.value);
        return false;
    }
    if (!ExpectedAction(definition.Get(), platform, expected)) {
        error = Problem(ru,
            L"Задача с именем продукта запускает другую программу или содержит дополнительные действия. Она не будет изменена.",
            L"A task with the product's name starts another program or has additional actions. It will not be changed.", path.value);
        return false;
    }
    if (FAILED(task->get_Xml(xml.out())) || !xml.value ||
        FAILED(task->GetSecurityDescriptor(kTaskSecurity, security.out())) || !security.value ||
        FAILED(task->get_Enabled(&enabled))) {
        error = Problem(ru,
            L"Не удалось сохранить определение задачи и её права. Установка остановлена до изменений.",
            L"Could not save the task definition and security. Setup stopped before making changes.", path.value);
        return false;
    }
    TaskSnapshot saved;
    saved.path = path.value; saved.xml = xml.value; saved.security = security.value;
    saved.enabled = enabled != VARIANT_FALSE;
    const std::wstring base = workspace + L"\\task-" + std::to_wstring(session.tasks.size());
    if (!WriteTextBackup(base + L".xml", saved.xml) || !WriteTextBackup(base + L".sddl", saved.security) ||
        !WriteTextBackup(base + L".path.txt", saved.path) ||
        !WriteTextBackup(base + L".enabled.txt", saved.enabled ? L"true" : L"false")) {
        error = Problem(ru, L"Не удалось сохранить резервную копию задачи.", L"Could not save the task backup.", saved.path, GetLastError());
        return false;
    }
    session.tasks.push_back(std::move(saved));
    return true;
}

// Search subfolders too: older OpenGlass versions locate these names recursively.
// If a folder cannot be inspected, fail rather than declare an incomplete
// removal or delete an unrelated same-named task.
bool ScanTasks(ITaskFolder* folder, bool x86, bool og, const std::wstring& x86Exe,
    const std::wstring& ogExe, Session& session, const std::wstring& workspace,
    const std::atomic_bool* cancel, bool ru, std::wstring& error, unsigned depth = 0,
    bool validateOnly = false) noexcept {
    if (Cancelled(cancel)) return false;
    if (depth > 64) {
        error = Problem(ru, L"Слишком большая вложенность папок планировщика.", L"Task Scheduler folder nesting is too deep.");
        return false;
    }
    Bstr currentFolder;
    (void)folder->get_Path(currentFolder.out());
    const std::wstring folderName=currentFolder.value ? currentFolder.value : L"";
    ComPtr<IRegisteredTaskCollection> tasks;
    HRESULT hr = folder->GetTasks(TASK_ENUM_HIDDEN, tasks.GetAddressOf());
    LONG count = 0;
    if (FAILED(hr) || !tasks || FAILED(tasks->get_Count(&count))) {
        error = Problem(ru, L"Не удалось проверить задачи планировщика. Изменения не внесены.",
            L"Could not inspect Task Scheduler tasks. No changes were made.", folderName, static_cast<unsigned>(hr));
        return false;
    }
    for (LONG i = 1; i <= count; ++i) {
        if (Cancelled(cancel)) return false;
        VARIANT index{}; VariantInit(&index); index.vt = VT_I4; index.lVal = i;
        ComPtr<IRegisteredTask> task; Bstr name;
        if (FAILED(tasks->get_Item(index, task.GetAddressOf())) || !task || FAILED(task->get_Name(name.out())) || !name.value) {
            error = Problem(ru, L"Не удалось прочитать задачу планировщика.", L"Could not read a scheduled task.");
            return false;
        }
        const bool isX86 = x86 && _wcsicmp(name.value, GP::GlassAutorunTaskName(GP::PlatformArch::X86)) == 0;
        const bool isOg = og && (_wcsicmp(name.value, L"OpenGlass Host") == 0 ||
                                _wcsicmp(name.value, L"OpenGlass Legacy Host") == 0);
        if (isX86 || isOg) {
            if (!validateOnly) {
                if (!CaptureTask(task.Get(),isX86,isX86?x86Exe:ogExe,session,workspace,ru,error)) return false;
            } else {
                ComPtr<ITaskDefinition> definition; Bstr taskPath;
                if (FAILED(task->get_Path(taskPath.out())) || !taskPath.value ||
                    FAILED(task->get_Definition(definition.GetAddressOf())) || !definition ||
                    !ExpectedAction(definition.Get(),isX86,isX86?x86Exe:ogExe)) {
                    error=Problem(ru,L"Задача запускает программу вне каталога установки или содержит неизвестные действия. Она не будет запущена.",
                        L"A task runs a program outside the installation directory or has unsupported actions. It will not be started.",
                        taskPath.value?taskPath.value:L"");
                    return false;
                }
            }
        }
    }
    // Platform autorun is created and looked up only in the root folder.
    // OpenGlass legacy discovery, in contrast, can locate tasks recursively.
    if (!og) return true;
    ComPtr<ITaskFolderCollection> folders;
    hr = folder->GetFolders(0, folders.GetAddressOf());
    if (FAILED(hr) || !folders || FAILED(folders->get_Count(&count))) {
        error = Problem(ru, L"Не удалось проверить папки планировщика. Изменения не внесены.",
            L"Could not inspect Task Scheduler folders. No changes were made.", folderName, static_cast<unsigned>(hr));
        return false;
    }
    for (LONG i = 1; i <= count; ++i) {
        if (Cancelled(cancel)) return false;
        VARIANT index{}; VariantInit(&index); index.vt = VT_I4; index.lVal = i;
        ComPtr<ITaskFolder> child;
        const HRESULT childResult=folders->get_Item(index, child.GetAddressOf());
        if (FAILED(childResult) || !child) {
            error = Problem(ru, L"Не удалось прочитать подпапку планировщика. Изменения не внесены.",
                L"Could not read a Task Scheduler subfolder. No changes were made.", folderName, static_cast<unsigned>(childResult));
            return false;
        }
        if (!ScanTasks(child.Get(), x86, og, x86Exe, ogExe, session, workspace, cancel, ru, error, depth + 1, validateOnly)) return false;
    }
    return true;
}

bool ReadServiceConfiguration(SC_HANDLE service, const std::wstring& expected, std::wstring& signature) noexcept {
    DWORD bytes = 0;
    (void)QueryServiceConfigW(service, nullptr, 0, &bytes);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !bytes || bytes > 1024 * 1024) return false;
    std::vector<BYTE> buffer(bytes);
    auto* config = reinterpret_cast<QUERY_SERVICE_CONFIGW*>(buffer.data());
    if (!QueryServiceConfigW(service, config, bytes, &bytes) || !config->lpBinaryPathName ||
        config->dwServiceType != SERVICE_WIN32_OWN_PROCESS) return false;
    // Unknown service-account passwords cannot be backed up. Setup creates
    // this product's own service under LocalSystem; leave other accounts alone.
    const wchar_t* account = config->lpServiceStartName;
    if (!account || (_wcsicmp(account, L"LocalSystem") != 0 &&
                     _wcsicmp(account, L"NT AUTHORITY\\SYSTEM") != 0)) return false;
    const std::wstring command = config->lpBinaryPathName;
    // An unquoted service path containing spaces is ambiguous; never guess.
    std::wstring executable;
    if (!command.empty() && command.front() == L'"') {
        const auto end = command.find(L'"', 1);
        if (end == std::wstring::npos) return false;
        executable = command.substr(1, end - 1);
    } else {
        const auto end = command.find_first_of(L" \t");
        executable = command.substr(0, end);
    }
    if (!SamePath(executable, expected)) return false;
    // The service is not modified until the final commit. Detect concurrent
    // changes to the core configuration rather than delete a changed service.
    signature.clear();
    const auto add = [&](const wchar_t* value) {
        if (value) signature.append(value);
        signature.push_back(L'\0');
    };
    add(config->lpBinaryPathName); add(config->lpLoadOrderGroup); add(config->lpServiceStartName); add(config->lpDisplayName);
    signature += std::to_wstring(config->dwServiceType) + L"/" + std::to_wstring(config->dwStartType) + L"/" +
        std::to_wstring(config->dwErrorControl) + L"/" + std::to_wstring(config->dwTagId);
    signature.push_back(L'\0');
    if (config->lpDependencies) {
        for (const wchar_t* dep=config->lpDependencies; *dep; dep+=wcslen(dep)+1) add(dep);
    }
    return true;
}

// Back up all registry configuration and security for manual recovery. The
// service itself remains untouched until the last irreversible commit step.
bool BackupServiceRegistry(const std::wstring& workspace, bool ru, std::wstring& error) noexcept {
    ScopedPrivilege backup(SE_BACKUP_NAME), security(SE_SECURITY_NAME);
    if (!backup.enabled || !security.enabled) {
        error = Problem(ru, L"Не удалось получить права для резервной копии службы. Изменения не внесены.",
            L"Could not obtain privileges to back up the service. No changes were made.");
        return false;
    }
    HKEY key = nullptr;
    const LSTATUS opened = RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Services\\OpenGlassHost",
        0, KEY_READ | ACCESS_SYSTEM_SECURITY | KEY_WOW64_64KEY, &key);
    if (opened != ERROR_SUCCESS) {
        error = Problem(ru, L"Не удалось сохранить настройки службы OpenGlassHost.",
            L"Could not back up the OpenGlassHost service configuration.", {}, opened);
        return false;
    }
    const std::wstring path = workspace + L"\\OpenGlassHost-service.hiv";
    const LSTATUS result = RegSaveKeyExW(key, path.c_str(), nullptr, REG_STANDARD_FORMAT);
    RegCloseKey(key);
    if (result != ERROR_SUCCESS) {
        error = Problem(ru, L"Не удалось сохранить настройки службы OpenGlassHost.",
            L"Could not back up the OpenGlassHost service configuration.", path, result);
        return false;
    }
    return true;
}

bool InspectService(Session& session, bool preflight, bool ru, std::wstring& error) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) {
        error = Problem(ru, L"Не удалось открыть диспетчер служб.", L"Could not open Service Control Manager.", {}, GetLastError());
        return false;
    }
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | DELETE);
    const DWORD openError = svc ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(scm);
    if (!svc) {
        if (openError == ERROR_SERVICE_DOES_NOT_EXIST && (preflight || !session.serviceExisted)) {
            if (preflight) session.serviceExisted = false;
            return true;
        }
        error = Problem(ru, L"Не удалось проверить службу OpenGlassHost или её состояние изменилось.",
            L"Could not inspect the OpenGlassHost service, or its state changed.", {}, openError);
        return false;
    }
    if (!preflight && !session.serviceExisted) {
        CloseServiceHandle(svc);
        error = Problem(ru, L"Во время установки появилась новая служба OpenGlassHost; она не будет изменена.",
            L"A new OpenGlassHost service appeared during setup; it will not be changed.");
        return false;
    }
    std::wstring configuration;
    const bool matches = ReadServiceConfiguration(svc, session.expectedServiceExe, configuration);
    CloseServiceHandle(svc);
    if (!matches) {
        error = Problem(ru, L"Служба OpenGlassHost не соответствует продукту или использует другую учётную запись. Она не будет изменена.",
            L"OpenGlassHost does not match the product or uses a different account. It will not be changed.", session.expectedServiceExe);
        return false;
    }
    if (preflight) { session.serviceExisted = true; session.serviceConfiguration = std::move(configuration); }
    else if (configuration != session.serviceConfiguration) {
        error = Problem(ru, L"Настройки службы OpenGlassHost изменились во время установки; она не будет удалена.",
            L"OpenGlassHost configuration changed during setup; it will not be deleted.");
        return false;
    }
    return true;
}

bool DeleteServiceLast(const Session& session, bool ru, std::wstring& error) noexcept {
    const SC_HANDLE scm = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT);
    if (!scm) { error = Problem(ru, L"Не удалось открыть диспетчер служб.", L"Could not open Service Control Manager.", {}, GetLastError()); return false; }
    const SC_HANDLE svc = OpenServiceW(scm, L"OpenGlassHost", SERVICE_QUERY_CONFIG | SERVICE_QUERY_STATUS | DELETE);
    const DWORD openError = svc ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(scm);
    if (!svc) {
        if (!session.serviceExisted && openError == ERROR_SERVICE_DOES_NOT_EXIST) return true;
        error = Problem(ru, L"Служба OpenGlassHost изменилась во время установки.", L"OpenGlassHost changed during setup.", {}, openError);
        return false;
    }
    SERVICE_STATUS_PROCESS status{}; DWORD needed = 0;
    std::wstring configuration;
    const bool safe = session.serviceExisted && ReadServiceConfiguration(svc, session.expectedServiceExe, configuration) &&
        configuration == session.serviceConfiguration &&
        QueryServiceStatusEx(svc, SC_STATUS_PROCESS_INFO, reinterpret_cast<BYTE*>(&status), sizeof(status), &needed) &&
        status.dwCurrentState == SERVICE_STOPPED;
    if (!safe) {
        CloseServiceHandle(svc);
        error = Problem(ru, L"Служба OpenGlassHost изменилась или не остановлена. Она не будет удалена.",
            L"OpenGlassHost changed or is still running. It will not be deleted.");
        return false;
    }
    const BOOL deleted = DeleteService(svc);
    const DWORD failure = deleted ? ERROR_SUCCESS : GetLastError();
    CloseServiceHandle(svc);
    if (!deleted) error = Problem(ru, L"Не удалось удалить службу OpenGlassHost.", L"Could not delete the OpenGlassHost service.", {}, failure);
    // IMPORTANT: no fallible work is allowed after a successful DeleteService.
    return deleted != FALSE;
}

bool SplitTaskPath(const std::wstring& path, std::wstring& folder, std::wstring& name) {
    const auto slash = path.find_last_of(L'\\');
    if (slash == std::wstring::npos || slash + 1 >= path.size()) return false;
    folder = slash == 0 ? L"\\" : path.substr(0, slash);
    name = path.substr(slash + 1);
    return true;
}

bool RemoveTask(ITaskService* service, TaskSnapshot& saved, bool ru, std::wstring& error) noexcept {
    std::wstring folderPath, name;
    ComPtr<ITaskFolder> folder;
    if (!SplitTaskPath(saved.path, folderPath, name) || !GetFolder(service, folderPath, folder)) {
        error = Problem(ru, L"Папка задачи изменилась во время установки.", L"The task folder changed during setup.", saved.path);
        return false;
    }
    Bstr taskName(name), xml, security;
    ComPtr<IRegisteredTask> current;
    VARIANT_BOOL enabled = VARIANT_FALSE;
    if (!taskName.value || FAILED(folder->GetTask(taskName.value, current.GetAddressOf())) || !current ||
        FAILED(current->get_Xml(xml.out())) || !xml.value ||
        FAILED(current->GetSecurityDescriptor(kTaskSecurity, security.out())) || !security.value ||
        FAILED(current->get_Enabled(&enabled)) || saved.xml != xml.value || saved.security != security.value ||
        saved.enabled != (enabled != VARIANT_FALSE)) {
        error = Problem(ru, L"Задача изменилась во время установки и не будет удалена.",
            L"The task changed during setup and will not be deleted.", saved.path);
        return false;
    }
    const HRESULT hr = folder->DeleteTask(taskName.value, 0);
    if (FAILED(hr)) {
        error = Problem(ru, L"Не удалось удалить задачу автозапуска.", L"Could not remove the startup task.", saved.path, static_cast<unsigned>(hr));
        return false;
    }
    saved.removed = true;
    GP::InvalidateGlassAutorunTaskCache();
    return true;
}

bool RestoreTask(ITaskService* service, TaskSnapshot& saved, bool ru, std::wstring& error) noexcept {
    if (!saved.removed) return true;
    std::wstring folderPath, name;
    ComPtr<ITaskFolder> folder;
    if (!SplitTaskPath(saved.path, folderPath, name) || !GetFolder(service, folderPath, folder)) {
        error = Problem(ru, L"Не удалось восстановить папку задачи.", L"Could not restore the task folder.", saved.path);
        return false;
    }
    Bstr taskName(name), xml(saved.xml), security(saved.security);
    if (!taskName.value || !xml.value || !security.value) return false;
    VARIANT empty{}; VariantInit(&empty);
    VARIANT sddl{}; VariantInit(&sddl); sddl.vt = VT_BSTR; sddl.bstrVal = security.value;
    ComPtr<IRegisteredTask> restored;
    // CREATE never replaces a concurrent task; retain its original ACL and do
    // not execute registration triggers while rolling back.
    const HRESULT hr = folder->RegisterTask(taskName.value, xml.value,
        TASK_CREATE | TASK_DONT_ADD_PRINCIPAL_ACE | TASK_IGNORE_REGISTRATION_TRIGGERS,
        empty, empty, TASK_LOGON_NONE, sddl, restored.GetAddressOf());
    if (FAILED(hr) || !restored || FAILED(restored->put_Enabled(saved.enabled ? VARIANT_TRUE : VARIANT_FALSE))) {
        error = Problem(ru, L"Не удалось восстановить задачу. Её XML и права сохранены в резервной копии.",
            L"Could not restore the task. Its XML and security remain in the backup.", saved.path, static_cast<unsigned>(hr));
        return false;
    }
    Bstr actualXml, actualSecurity;
    VARIANT_BOOL actualEnabled = VARIANT_FALSE;
    if (FAILED(restored->get_Xml(actualXml.out())) || !actualXml.value || saved.xml != actualXml.value ||
        FAILED(restored->GetSecurityDescriptor(kTaskSecurity, actualSecurity.out())) || !actualSecurity.value ||
        saved.security != actualSecurity.value || FAILED(restored->get_Enabled(&actualEnabled)) ||
        saved.enabled != (actualEnabled != VARIANT_FALSE)) {
        error = Problem(ru, L"Восстановленная задача отличается от копии. Проверьте её XML и права в резервной папке.",
            L"The restored task differs from its backup. Check its XML and security in the backup folder.", saved.path);
        return false;
    }
    saved.removed = false;
    GP::InvalidateGlassAutorunTaskCache();
    return true;
}

bool ReadFileSecurity(HANDLE file, std::vector<BYTE>& out) noexcept {
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const DWORD status = GetSecurityInfo(file, SE_FILE_OBJECT, kFileSecurity,
        nullptr, nullptr, nullptr, nullptr, &descriptor);
    if (status != ERROR_SUCCESS || !descriptor) { SetLastError(status); return false; }
    const DWORD size = GetSecurityDescriptorLength(descriptor);
    if (!size) { LocalFree(descriptor); return false; }
    const BYTE* bytes = static_cast<const BYTE*>(descriptor);
    out.assign(bytes, bytes + size);
    LocalFree(descriptor);
    return true;
}

bool WriteSecurityBackup(const std::wstring& path, const std::vector<BYTE>& bytes) noexcept {
    if (bytes.empty() || bytes.size() > MAXDWORD) return false;
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file);
    return ok;
}

bool RestoreFileSecurity(const std::wstring& path, const std::vector<BYTE>& bytes) noexcept {
    if (bytes.empty()) return false;
    ScopedPrivilege security(SE_SECURITY_NAME), restore(SE_RESTORE_NAME);
    if (!security.enabled || !restore.enabled) return false;
    // The restored descriptor is read back through this same handle below.
    const HANDLE file = CreateFileW(path.c_str(), READ_CONTROL | WRITE_DAC | WRITE_OWNER | ACCESS_SYSTEM_SECURITY,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BY_HANDLE_FILE_INFORMATION info{};
    bool ok = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT));
    auto* descriptor = reinterpret_cast<PSECURITY_DESCRIPTOR>(const_cast<BYTE*>(bytes.data()));
    PSID owner = nullptr, group = nullptr;
    PACL dacl = nullptr, sacl = nullptr;
    BOOL defaulted = FALSE, daclPresent = FALSE, saclPresent = FALSE;
    SECURITY_DESCRIPTOR_CONTROL control{}; DWORD revision = 0;
    ok = ok && IsValidSecurityDescriptor(descriptor) &&
        GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) &&
        GetSecurityDescriptorGroup(descriptor, &group, &defaulted) &&
        GetSecurityDescriptorDacl(descriptor, &daclPresent, &dacl, &defaulted) &&
        GetSecurityDescriptorSacl(descriptor, &saclPresent, &sacl, &defaulted) &&
        GetSecurityDescriptorControl(descriptor, &control, &revision);
    if (ok) {
        SECURITY_INFORMATION flags = kFileSecurity |
            ((control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION) |
            ((control & SE_SACL_PROTECTED) ? PROTECTED_SACL_SECURITY_INFORMATION : UNPROTECTED_SACL_SECURITY_INFORMATION);
        const DWORD status = SetSecurityInfo(file, SE_FILE_OBJECT, flags, owner, group,
            daclPresent ? dacl : nullptr, saclPresent ? sacl : nullptr);
        ok = status == ERROR_SUCCESS;
        if (!ok) SetLastError(status);
        else {
            std::vector<BYTE> actual;
            ok = ReadFileSecurity(file, actual) && actual == bytes;
            if (!ok) SetLastError(ERROR_INVALID_SECURITY_DESCR);
        }
    }
    CloseHandle(file);
    return ok;
}

bool PrepareShortcut(Session& session, const std::wstring& workspace,
    const std::wstring& x64Dir, const std::wstring& x86Dir, bool ru, std::wstring& error) noexcept {
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs)) || !programs) {
        error = Problem(ru, L"Не удалось проверить ярлык GlassSettings.", L"Could not inspect the GlassSettings shortcut.");
        return false;
    }
    const std::wstring folder = std::wstring(programs) + L"\\GlassPlatform";
    CoTaskMemFree(programs);
    session.shortcutPath = folder + L"\\GlassSettings.lnk";
    const DWORD folderAttr = GetFileAttributesW(folder.c_str());
    if (folderAttr != INVALID_FILE_ATTRIBUTES && (folderAttr & FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = Problem(ru, L"Папка ярлыка является ссылкой; она не будет изменена.", L"The shortcut folder is a reparse point; it will not be changed.", folder);
        return false;
    }
    const DWORD attr = GetFileAttributesW(session.shortcutPath.c_str());
    if (attr == INVALID_FILE_ATTRIBUTES) {
        const DWORD failure = GetLastError();
        if (failure == ERROR_FILE_NOT_FOUND || failure == ERROR_PATH_NOT_FOUND) return true;
        error = Problem(ru, L"Не удалось проверить ярлык GlassSettings.", L"Could not inspect the GlassSettings shortcut.", session.shortcutPath, failure);
        return false;
    }
    if (attr & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) {
        error = Problem(ru, L"Ярлык является ссылкой на файловую систему или каталогом; он не будет изменён.",
            L"The shortcut is a reparse point or directory; it will not be changed.", session.shortcutPath);
        return false;
    }
    ScopedPrivilege privilege(SE_SECURITY_NAME);
    if (!privilege.enabled) {
        error = Problem(ru, L"Не удалось сохранить права ярлыка. Изменения не внесены.",
            L"Could not save the shortcut security. No changes were made.", session.shortcutPath);
        return false;
    }
    const HANDLE file = CreateFileW(session.shortcutPath.c_str(), GENERIC_READ | READ_CONTROL | ACCESS_SYSTEM_SECURITY,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = Problem(ru, L"Не удалось сохранить ярлык и его права.", L"Could not back up the shortcut and its security.", session.shortcutPath, GetLastError());
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    session.shortcutBackup = workspace + L"\\GlassSettings.lnk";
    const bool saved = GetFileInformationByHandle(file, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ReadFileSecurity(file, session.shortcutSecurity) &&
        WriteSecurityBackup(workspace + L"\\GlassSettings.security.bin", session.shortcutSecurity) &&
        CopyFileW(session.shortcutPath.c_str(), session.shortcutBackup.c_str(), TRUE);
    const DWORD failure = saved ? ERROR_SUCCESS : GetLastError();
    CloseHandle(file);
    if (!saved) {
        error = Problem(ru, L"Не удалось сохранить ярлык и его права.", L"Could not back up the shortcut and its security.", session.shortcutPath, failure);
        return false;
    }
    ComGuard com;
    ComPtr<IShellLinkW> link;
    ComPtr<IPersistFile> persist;
    wchar_t target[32768]{};
    if (!com.ok() || FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
            IID_IShellLinkW, reinterpret_cast<void**>(link.GetAddressOf()))) || !link ||
        FAILED(link->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(persist.GetAddressOf()))) || !persist ||
        FAILED(persist->Load(session.shortcutBackup.c_str(), STGM_READ)) ||
        FAILED(link->GetPath(target, static_cast<int>(std::size(target)), nullptr, SLGP_RAWPATH)) || !target[0]) {
        error = Problem(ru, L"Не удалось проверить назначение ярлыка GlassSettings. Он не будет изменён.",
            L"Could not inspect the GlassSettings shortcut target. It will not be changed.", session.shortcutPath);
        return false;
    }
    const auto targetIn = [&](const std::wstring& root) {
        return !root.empty() && SamePath(target, GlassInstallUtil::TrimTrailingSlashes(root) + L"\\GlassSettings\\GlassSettings.exe");
    };
    if (!targetIn(x64Dir) && !targetIn(x86Dir)) {
        error = Problem(ru, L"Ярлык GlassSettings указывает на другую программу. Он не будет изменён.",
            L"The GlassSettings shortcut points to a different program. It will not be changed.", session.shortcutPath);
        return false;
    }
    session.shortcutExisted = true;
    return true;
}

bool SameOpenFiles(HANDLE left, HANDLE right) noexcept {
    LARGE_INTEGER a{}, b{};
    if (!GetFileSizeEx(left, &a) || !GetFileSizeEx(right, &b) || a.QuadPart != b.QuadPart) return false;
    BYTE x[4096], y[4096];
    for (;;) {
        DWORD nx = 0, ny = 0;
        if (!ReadFile(left, x, sizeof(x), &nx, nullptr) || !ReadFile(right, y, sizeof(y), &ny, nullptr) ||
            nx != ny || (nx && std::memcmp(x, y, nx) != 0)) return false;
        if (!nx) return true;
    }
}

bool RemoveShortcut(Session& session, bool ru, std::wstring& error) noexcept {
    if (!session.shortcutExisted) return true;
    ScopedPrivilege privilege(SE_SECURITY_NAME);
    if (!privilege.enabled) {
        error = Problem(ru, L"Не удалось проверить права ярлыка перед удалением.",
            L"Could not check shortcut security before removing it.", session.shortcutPath);
        return false;
    }
    const HANDLE link = CreateFileW(session.shortcutPath.c_str(), GENERIC_READ | DELETE | READ_CONTROL | ACCESS_SYSTEM_SECURITY, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    const HANDLE backup = CreateFileW(session.shortcutBackup.c_str(), GENERIC_READ, FILE_SHARE_READ,
        nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (link == INVALID_HANDLE_VALUE || backup == INVALID_HANDLE_VALUE) {
        if (link != INVALID_HANDLE_VALUE) CloseHandle(link);
        if (backup != INVALID_HANDLE_VALUE) CloseHandle(backup);
        error = Problem(ru, L"Не удалось проверить ярлык перед удалением.", L"Could not check the shortcut before removing it.", session.shortcutPath);
        return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    std::vector<BYTE> security;
    const bool same = GetFileInformationByHandle(link, &info) &&
        !(info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) &&
        ReadFileSecurity(link, security) && security == session.shortcutSecurity && SameOpenFiles(link, backup);
    FILE_DISPOSITION_INFO disposition{}; disposition.DeleteFile = TRUE;
    const bool removed = same && SetFileInformationByHandle(link, FileDispositionInfo, &disposition, sizeof(disposition));
    const DWORD failure = removed ? ERROR_SUCCESS : GetLastError();
    CloseHandle(backup); CloseHandle(link);
    if (!removed) {
        error = Problem(ru, L"Ярлык изменился или не может быть удалён.", L"The shortcut changed or could not be removed.", session.shortcutPath, failure);
        return false;
    }
    session.shortcutRemoved = true;
    return true;
}
} // namespace

namespace GlassInstallRemoval {
bool ValidatePlatformTask(GP::PlatformArch arch, const std::wstring& fixedRoot,
    bool expectedExists, bool requireRunning, bool russian, std::wstring& error) noexcept {
    error.clear();
    if ((!fixedRoot.empty() && !GlassInstallUtil::IsSafeInstallPath(
            GlassInstallUtil::TrimTrailingSlashes(fixedRoot)+L"\\GlassCompat.dll")) ||
        (fixedRoot.empty() && (expectedExists || requireRunning))) {
        error=Problem(russian,L"Небезопасный путь платформы или его родительский каталог.",
            L"The platform path or an ancestor is unsafe.",fixedRoot);
        return false;
    }
    ComGuard com; ComPtr<ITaskService> service; ComPtr<ITaskFolder> root;
    if (!com.ok() || !Connect(service) || !GetFolder(service.Get(),L"\\",root)) {
        error=Problem(russian,L"Не удалось проверить задачу платформы. Установка остановлена.",
            L"Could not inspect the platform task. Setup stopped.");
        return false;
    }
    ComPtr<IRegisteredTaskCollection> tasks; LONG count=0;
    if (FAILED(root->GetTasks(TASK_ENUM_HIDDEN,tasks.GetAddressOf())) || !tasks ||
        FAILED(tasks->get_Count(&count)) || count<0) {
        error=Problem(russian,L"Не удалось прочитать задачи платформы.",L"Could not read platform tasks.");
        return false;
    }
    bool found=false;
    const auto expected=GlassInstallUtil::TrimTrailingSlashes(fixedRoot)+L"\\GlassCompat.dll";
    for (LONG i=1;i<=count;++i) {
        VARIANT index{};VariantInit(&index);index.vt=VT_I4;index.lVal=i;
        ComPtr<IRegisteredTask> task;Bstr name;
        if (FAILED(tasks->get_Item(index,task.GetAddressOf())) || !task ||
            FAILED(task->get_Name(name.out())) || !name.value) {
            error=Problem(russian,L"Не удалось прочитать имя задачи.",L"Could not read a task name.");
            return false;
        }
        if (_wcsicmp(name.value,GP::GlassAutorunTaskName(arch))!=0) continue;
        found=true;
        ComPtr<ITaskDefinition> definition;
        if (FAILED(task->get_Definition(definition.GetAddressOf())) || !definition ||
            !ExpectedAction(definition.Get(),true,expected,arch)) {
            error=Problem(russian,L"Задача платформы не соответствует пути установки или содержит неизвестные действия. Она не будет запущена или изменена.",
                L"The platform task points outside the installation directory or has unsupported actions. It will not be started or changed.",name.value);
            return false;
        }
        if (requireRunning) {
            ComPtr<IRunningTaskCollection> instances;LONG running=0;
            if (FAILED(task->GetInstances(0,instances.GetAddressOf())) || !instances ||
                FAILED(instances->get_Count(&running)) || running!=1) {
                error=Problem(russian,L"Работающий экземпляр платформы не удалось однозначно связать с задачей. Остановите его вручную и повторите установку.",
                    L"The running platform could not be associated with exactly one task instance. Stop it manually and run Setup again.",name.value);
                return false;
            }
        }
    }
    if (found!=expectedExists || (requireRunning && !found)) {
        error=Problem(russian,L"Задача платформы появилась или исчезла во время проверки. Повторите установку.",
            L"The platform task appeared or disappeared during the check. Run Setup again.",GP::GlassAutorunTaskName(arch));
        return false;
    }
    return true;
}

bool StopValidatedPlatformTask(GP::PlatformArch arch, const std::wstring& fixedRoot,
    const std::atomic_bool* cancelled, bool& stopAttempted, bool russian, std::wstring& error) noexcept {
    stopAttempted=false;
    error.clear();
    if (Cancelled(cancelled)) { SetLastError(ERROR_CANCELLED); return false; }
    if (!ValidatePlatformTask(arch,fixedRoot,true,true,russian,error)) return false;
    if (Cancelled(cancelled)) { SetLastError(ERROR_CANCELLED); return false; }

    // Reopen and recheck the exact object that receives Stop. A task may have
    // changed after the earlier snapshot or between the initial check and now.
    ComGuard com; ComPtr<ITaskService> service; ComPtr<ITaskFolder> root;
    Bstr name(GP::GlassAutorunTaskName(arch));
    ComPtr<IRegisteredTask> task;
    const auto expected=GlassInstallUtil::TrimTrailingSlashes(fixedRoot)+L"\\GlassCompat.dll";
    if (!com.ok() || !name.value || !GlassInstallUtil::IsSafeInstallPath(expected) ||
        !Connect(service) || !GetFolder(service.Get(),L"\\",root) ||
        FAILED(root->GetTask(name.value,task.GetAddressOf())) || !task) {
        error=Problem(russian,L"Не удалось повторно проверить задачу платформы перед остановкой.",
            L"Could not recheck the platform task before stopping it.",GP::GlassAutorunTaskName(arch));
        return false;
    }
    ComPtr<ITaskDefinition> definition;
    ComPtr<IRunningTaskCollection> instances; LONG running=0;
    if (FAILED(task->get_Definition(definition.GetAddressOf())) || !definition ||
        !ExpectedAction(definition.Get(),true,expected,arch) ||
        FAILED(task->GetInstances(0,instances.GetAddressOf())) || !instances ||
        FAILED(instances->get_Count(&running)) || running!=1) {
        error=Problem(russian,L"Задача платформы или её экземпляр изменились. Остановка отменена.",
            L"The platform task or its instance changed. Stop was not requested.",GP::GlassAutorunTaskName(arch));
        return false;
    }
    if (Cancelled(cancelled)) { SetLastError(ERROR_CANCELLED); return false; }

    // IRegisteredTask::Stop is the Task Scheduler operation used to end a
    // task. S_FALSE is not success: it means the instances could not be stopped.
    stopAttempted=true;
    const HRESULT result=task->Stop(0);
    GlassInstallLog::LineFormat(L"TaskStop: %s IRegisteredTask::Stop hr=0x%08lX",
        GP::GlassAutorunTaskName(arch),static_cast<unsigned long>(result));
    if (result!=S_OK) {
        error=Problem(russian,L"Планировщик не подтвердил остановку задачи платформы.",
            L"Task Scheduler did not confirm the platform task was stopped.",
            GP::GlassAutorunTaskName(arch),static_cast<unsigned>(result));
        SetLastError(result==S_FALSE ? ERROR_ACCESS_DENIED : ERROR_GEN_FAILURE);
        return false;
    }
    return true;
}

bool ValidateOpenGlassPaths(const std::wstring& fixedRoot, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error) noexcept {
    error.clear();
    if (Cancelled(cancelled)) return false;
    const auto expected=GlassInstallUtil::TrimTrailingSlashes(fixedRoot)+L"\\OpenGlassHost.exe";
    if (fixedRoot.empty() || !GlassInstallUtil::IsSafeInstallPath(expected)) {
        error=Problem(russian,L"Небезопасный путь OpenGlass или его родительский каталог.",
            L"The OpenGlass path or an ancestor is unsafe.",fixedRoot);
        return false;
    }
    const SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT);
    if (!scm) {
        error=Problem(russian,L"Не удалось проверить службу OpenGlassHost.",L"Could not inspect OpenGlassHost.",{},GetLastError());
        return false;
    }
    const SC_HANDLE service=OpenServiceW(scm,L"OpenGlassHost",SERVICE_QUERY_CONFIG|SERVICE_QUERY_STATUS);
    const DWORD openError=service?ERROR_SUCCESS:GetLastError();
    CloseServiceHandle(scm);
    if (!service && openError!=ERROR_SERVICE_DOES_NOT_EXIST) {
        error=Problem(russian,L"Не удалось проверить службу OpenGlassHost.",L"Could not inspect OpenGlassHost.",{},openError);
        return false;
    }
    if (service) {
        std::wstring signature;
        bool matches=ReadServiceConfiguration(service,expected,signature);
        SERVICE_STATUS_PROCESS status{};DWORD needed=0;
        matches=matches && QueryServiceStatusEx(service,SC_STATUS_PROCESS_INFO,
            reinterpret_cast<BYTE*>(&status),sizeof(status),&needed)!=FALSE;
        if (matches && status.dwCurrentState!=SERVICE_STOPPED) {
            // The configured path alone does not prove which executable was
            // already running when somebody changed the service configuration.
            const HANDLE process=status.dwProcessId?OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,FALSE,status.dwProcessId):nullptr;
            std::vector<wchar_t> image(32768);DWORD size=static_cast<DWORD>(image.size());
            const bool known=process && QueryFullProcessImageNameW(process,0,image.data(),&size);
            if (process) CloseHandle(process);
            matches=known && SamePath(image.data(),expected);
        }
        CloseServiceHandle(service);
        if (!matches) {
            error=Problem(russian,L"Служба OpenGlassHost не соответствует пути установки или её конфигурацию нельзя проверить. Она не будет запущена или изменена.",
                L"OpenGlassHost points outside the installation directory or its configuration cannot be verified. It will not be started or changed.",expected);
            return false;
        }
    }
    ComGuard com;ComPtr<ITaskService> taskService;ComPtr<ITaskFolder> root;Session unused;
    if (!com.ok() || !Connect(taskService) || !GetFolder(taskService.Get(),L"\\",root)) {
        error=Problem(russian,L"Не удалось проверить задачи OpenGlass.",L"Could not inspect OpenGlass tasks.");
        return false;
    }
    return ScanTasks(root.Get(),false,true,{},expected,unused,{},cancelled,russian,error,0,true);
}

bool Prepare(Session& session, bool removeX86, bool removeOpenGlass, bool removeSettings,
    const std::wstring& x64Dir, const std::wstring& x86Dir, const std::wstring& openGlassDir,
    const std::wstring& secureWorkspace, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error) noexcept {
    session = {}; error.clear();
    if (Cancelled(cancelled)) return false;
    if (removeSettings && !PrepareShortcut(session, secureWorkspace, x64Dir, x86Dir, russian, error)) return false;
    if (removeOpenGlass) {
        session.expectedServiceExe = GlassInstallUtil::TrimTrailingSlashes(openGlassDir) + L"\\OpenGlassHost.exe";
        if (!InspectService(session, true, russian, error)) return false;
        if (session.serviceExisted && !BackupServiceRegistry(secureWorkspace, russian, error)) return false;
    }
    if (removeX86 || removeOpenGlass) {
        ComGuard com; ScopedPrivilege privilege(SE_SECURITY_NAME);
        ComPtr<ITaskService> service; ComPtr<ITaskFolder> root;
        if (!com.ok() || !privilege.enabled || !Connect(service) || !GetFolder(service.Get(), L"\\", root)) {
            error = Problem(russian, L"Не удалось безопасно прочитать задачи и их права. Установка остановлена до изменений.",
                L"Could not safely read tasks and their security. Setup stopped before making changes.");
            return false;
        }
        const std::wstring x86Exe = GlassInstallUtil::TrimTrailingSlashes(x86Dir) + L"\\GlassCompat.dll";
        const std::wstring ogExe = GlassInstallUtil::TrimTrailingSlashes(openGlassDir) + L"\\OpenGlassHost.exe";
        if (!ScanTasks(root.Get(), removeX86, removeOpenGlass, x86Exe, ogExe, session,
                secureWorkspace, cancelled, russian, error)) return false;
    }
    return !Cancelled(cancelled);
}

bool Commit(Session& session, bool russian, std::wstring& error) noexcept {
    error.clear();
    const auto orderedCommit = [&](ITaskService* service) {
        return GlassRemovalOrder::Commit(session.tasks.size(),
            [&] { return RemoveShortcut(session, russian, error); },
            [&](size_t i) { return service && RemoveTask(service, session.tasks[i], russian, error); },
            [&] {
                // The service is never recreated: credentials and arbitrary
                // configuration cannot generally be recovered. Delete it last.
                return session.expectedServiceExe.empty() || DeleteServiceLast(session, russian, error);
            });
    };
    if (session.tasks.empty()) return orderedCommit(nullptr);
    ComGuard com; ScopedPrivilege privilege(SE_SECURITY_NAME);
    ComPtr<ITaskService> service;
    if (!com.ok() || !privilege.enabled || !Connect(service)) {
        error = Problem(russian, L"Не удалось открыть планировщик задач.", L"Could not open Task Scheduler.");
        return false;
    }
    return orderedCommit(service.Get());
}

bool Restore(Session& session, bool russian, std::wstring& error) noexcept {
    error.clear();
    bool ok = true;
    bool tasksRemoved = false;
    for (const auto& task : session.tasks) tasksRemoved = tasksRemoved || task.removed;
    if (tasksRemoved) {
        ComGuard com; ScopedPrivilege privilege(SE_SECURITY_NAME);
        ComPtr<ITaskService> service;
        if (!com.ok() || !privilege.enabled || !Connect(service)) {
            error = Problem(russian, L"Не удалось открыть планировщик для восстановления задач.",
                L"Could not open Task Scheduler to restore tasks.");
            ok = false;
        } else {
            for (auto it = session.tasks.rbegin(); it != session.tasks.rend(); ++it) {
                std::wstring detail;
                if (!RestoreTask(service.Get(), *it, russian, detail)) { ok = false; if (error.empty()) error = detail; }
            }
        }
    }
    if (session.shortcutRemoved) {
        if (!CopyFileW(session.shortcutBackup.c_str(), session.shortcutPath.c_str(), TRUE)) {
            if (error.empty()) error = Problem(russian, L"Не удалось восстановить ярлык GlassSettings.",
                L"Could not restore the GlassSettings shortcut.", session.shortcutPath, GetLastError());
            ok = false;
        } else if (!RestoreFileSecurity(session.shortcutPath, session.shortcutSecurity)) {
            if (error.empty()) error = Problem(russian, L"Ярлык восстановлен, но не удалось восстановить его права.",
                L"The shortcut was restored, but its security could not be restored.", session.shortcutPath, GetLastError());
            ok = false;
        } else session.shortcutRemoved = false;
    }
    if (!ok) GlassInstallLog::LineFormat(L"Removal rollback failed: %s", error.c_str());
    return ok;
}
} // namespace GlassInstallRemoval
