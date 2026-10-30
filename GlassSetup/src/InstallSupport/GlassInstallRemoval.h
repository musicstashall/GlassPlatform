#pragma once

#include <windows.h>
#include <GlassPlatformTask.h>
#include <atomic>
#include <string>
#include <vector>

// Reversible cleanup for products removed by Setup. The MSI/file snapshot is
// handled separately by GlassInstallRollback. Do not use the uninstaller's
// unconditional service/task deletion here: it cannot restore their settings.
namespace GlassInstallRemoval {

struct TaskSnapshot {
    std::wstring path;
    std::wstring xml;
    std::wstring security;
    bool enabled = false;
    bool removed = false;
};

struct Session {
    std::vector<TaskSnapshot> tasks;
    std::wstring shortcutPath;
    std::wstring shortcutBackup;
    std::vector<BYTE> shortcutSecurity;
    std::wstring expectedServiceExe;
    std::wstring serviceConfiguration;
    bool shortcutExisted = false;
    bool shortcutRemoved = false;
    bool serviceExisted = false;
};

// Read-only checks used before touching or starting managed backends. Refuse
// unexpected task actions or service executables instead of executing them.
bool ValidatePlatformTask(GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& fixedRoot, bool expectedExists, bool requireRunning,
    bool russian, std::wstring& error) noexcept;
bool ValidateOpenGlassPaths(const std::wstring& fixedRoot, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error) noexcept;

// Prepare before stopping hosts or changing MSI registrations. Only remove
// auxiliaries of products known to have been installed. Refuse unknown task
// credentials or foreign service/task actions rather than destroy them.
bool Prepare(Session& session, bool removeX86, bool removeOpenGlass, bool removeSettings,
    const std::wstring& x64Dir, const std::wstring& x86Dir, const std::wstring& openGlassDir,
    const std::wstring& secureWorkspace, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error) noexcept;

// Called after launching/verifying selected products, with cancellation closed.
// Deleting the service is the LAST fallible operation: DeleteService cannot
// preserve arbitrary service credentials if it later has to be recreated.
bool Commit(Session& session, bool russian, std::wstring& error) noexcept;

// Restore any task/shortcut removed before a failed commit. Never overwrite an
// object which appeared or changed concurrently. Backups stay in the secured
// rollback workspace if this fails.
bool Restore(Session& session, bool russian, std::wstring& error) noexcept;

} // namespace GlassInstallRemoval
