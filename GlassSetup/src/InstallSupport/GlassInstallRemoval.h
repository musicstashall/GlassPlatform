#pragma once

#include <windows.h>
#include <GlassPlatformTask.h>
#include <atomic>
#include <string>
#include <vector>
#include <utility>

// Validation of managed tasks/service paths (used by Setup and GlassUnins) and
// a reversible cleanup session for Glass product auxiliaries in mixed Setup
// plans. Snapshot before stopping hosts, commit last, restore on rollback.
// Setup's original-vendor SUT removal uses a separate commit-last MSI path. The MSI/file snapshot is handled separately by
// GlassInstallRollback. GlassUnins reuses the shortcut-only retarget session,
// not Setup's MSI/file rollback. Do not use the uninstaller's unconditional
// service/task deletion here: it cannot restore their settings.
namespace GlassInstallRemoval {

struct TaskSnapshot {
    std::wstring path;
    std::wstring xml;
    std::wstring security;
    bool enabled = false;
    bool removed = false;
};

// Deny concurrent writers/deleters for the entire reversible shortcut edit.
struct ShortcutLock {
    HANDLE file=INVALID_HANDLE_VALUE;
    ShortcutLock()=default;
    ShortcutLock(const ShortcutLock&)=delete;
    ShortcutLock& operator=(const ShortcutLock&)=delete;
    ShortcutLock(ShortcutLock&& other) noexcept : file(std::exchange(other.file,INVALID_HANDLE_VALUE)) {}
    ShortcutLock& operator=(ShortcutLock&& other) noexcept {
        if(this!=&other){reset();file=std::exchange(other.file,INVALID_HANDLE_VALUE);}return *this;
    }
    ~ShortcutLock(){reset();}
    void reset() noexcept {if(file!=INVALID_HANDLE_VALUE)CloseHandle(std::exchange(file,INVALID_HANDLE_VALUE));}
};

struct Session {
    std::vector<TaskSnapshot> tasks;
    std::wstring shortcutPath;
    std::wstring shortcutBackup;
    std::vector<BYTE> shortcutSecurity;
    std::wstring expectedServiceExe;
    std::wstring serviceConfiguration;
    ShortcutLock shortcutLock;
    std::wstring shortcutRetargetBackup,shortcutRetargetExe;
    FILE_BASIC_INFO shortcutBasic{};
    bool shortcutPreserved=false,shortcutRetargetAttempted=false;
    bool shortcutExisted = false;
    bool shortcutRemoved = false;
    bool serviceExisted = false;
};

// Read-only checks used before touching or starting managed backends. Refuse
// unexpected task actions or service executables instead of executing them.
bool ValidatePlatformTask(GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& fixedRoot, bool expectedExists, bool requireRunning,
    bool russian, std::wstring& error) noexcept;
// Validate the task's action and single running instance on the same Scheduler
// object immediately before requesting Stop. No event or process fallback.
bool StopValidatedPlatformTask(GlassShared::GlassPlatformTask::PlatformArch arch,
    const std::wstring& fixedRoot, const std::atomic_bool* cancelled,
    bool& stopAttempted, bool russian, std::wstring& error) noexcept;
bool ValidateOpenGlassPaths(const std::wstring& fixedRoot, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error) noexcept;

// Prepare before stopping hosts or changing MSI registrations. Only remove
// auxiliaries of products known to have been installed. Refuse unknown task
// credentials or foreign service/task actions rather than destroy them.
bool Prepare(Session& session, bool removeX86, bool removeOpenGlass, bool removeSettings,
    const std::wstring& x64Dir, const std::wstring& x86Dir, const std::wstring& openGlassDir,
    const std::wstring& secureWorkspace, const std::atomic_bool* cancelled,
    bool russian, std::wstring& error, bool removeX64=false) noexcept;

// Snapshot/lock the existing owned shortcut, serialize a modified private copy.
// No shared link bytes are changed before Commit; a missing/custom link is not
// replaced by a generic new link. Caller supplies the surviving protected root.
bool PrepareSettingsShortcutRetarget(Session& session,const std::wstring& survivingRoot,
    const std::wstring& x64Dir,const std::wstring& x86Dir,const std::wstring& secureWorkspace,
    bool russian,std::wstring& error) noexcept;

// Commit with cancellation closed: Setup after rollbackable MSI/registration
// work, before launching; GlassUnins shortcut-only sessions before any MSI.
// Deleting the service is the LAST fallible operation: DeleteService cannot
// preserve arbitrary service credentials if it later has to be recreated.
bool Commit(Session& session, bool russian, std::wstring& error) noexcept;

// Restore any task/shortcut removed before a failed commit. Never overwrite an
// object which appeared or changed concurrently. Backups stay in the secured
// rollback workspace if this fails.
bool Restore(Session& session, bool russian, std::wstring& error) noexcept;

} // namespace GlassInstallRemoval
