#pragma once

// Default per-user process lists of Glass Platform.
//
// Setup seeds them into the current user's hive on a fresh installation and
// Repair rewrites them (together with the rest of the user key) to the
// fresh-install state. Both keys live under
//   HKCU\Software\StanislavKa\GlassPlatform
// and are read by the platform itself:
//   AttachList  — GlassCompat/src/AttachList.cpp: one REG_DWORD per executable,
//                 value = Shared::InjectionMode (0 Forbidden, 1 PopupOnly,
//                 2 Full); names are compared case-insensitively. Unknown
//                 executables default to PopupOnly, a few CoreWindow hosts are
//                 hard-coded to Full.
//   BlockList   — GlassShared/src/ProcessBlockList.cpp: one REG_DWORD per
//                 executable, any non-zero value blocks the module load; the
//                 built-in kBuiltInNames list is always applied in addition.
//
// The header is data only and must stay buildable without <windows.h> so the
// installer's host-side tests can include it.

namespace GlassShared::DefaultProcessLists
{
    inline constexpr const wchar_t* kUserKey = L"Software\\StanislavKa\\GlassPlatform";
    inline constexpr const wchar_t* kAttachListSubkey = L"Software\\StanislavKa\\GlassPlatform\\AttachList";
    inline constexpr const wchar_t* kBlockListSubkey = L"Software\\StanislavKa\\GlassPlatform\\BlockList";

    // Numeric values of Shared::InjectionMode (GlassShared/include/InjectionMode.h).
    inline constexpr unsigned kModeForbidden = 0;
    inline constexpr unsigned kModePopupOnly = 1;
    inline constexpr unsigned kModeFull = 2;

    struct AttachEntry
    {
        const wchar_t* exe;
        unsigned mode;
    };

    inline constexpr AttachEntry kAttachList[] = {
        // Forbidden: console and plain editors are never touched.
        { L"cmd.exe", kModeForbidden },
        { L"notepad.exe", kModeForbidden },

        // PopupOnly: menus and popups only, no full window treatment.
        { L"AeroGlassGUI.exe", kModePopupOnly },
        { L"charmap.exe", kModePopupOnly },
        { L"cttune.exe", kModePopupOnly },
        { L"DeviceProperties.exe", kModePopupOnly },
        { L"FlashPlayerApp.exe", kModePopupOnly },
        { L"hh.exe", kModePopupOnly },
        { L"ImagingDevices.exe", kModePopupOnly },
        { L"IMJPDCT.EXE", kModePopupOnly },
        { L"IMJPSET.EXE", kModePopupOnly },
        { L"MdSched.exe", kModePopupOnly },
        { L"msdt.exe", kModePopupOnly },
        { L"OpenWith.exe", kModePopupOnly },
        { L"OptionalFeatures.exe", kModePopupOnly },
        { L"PresentationSettings.exe", kModePopupOnly },
        { L"psr.exe", kModePopupOnly },
        { L"pwcreator.exe", kModePopupOnly },
        { L"rekeywiz.exe", kModePopupOnly },
        { L"rstrui.exe", kModePopupOnly },
        { L"SystemPropertiesComputerName.exe", kModePopupOnly },
        { L"SystemPropertiesPerformance.exe", kModePopupOnly },
        { L"SystemPropertiesProtection.exe", kModePopupOnly },
        { L"SystemPropertiesRemote.exe", kModePopupOnly },
        { L"taskhostw.exe", kModePopupOnly },
        { L"Taskmgr.exe", kModePopupOnly },
        { L"uTorrent.exe", kModePopupOnly },
        { L"Windows Style Builder.exe", kModePopupOnly },
        { L"WorkShelf.exe", kModePopupOnly },
        { L"xwizard.exe", kModePopupOnly },

        // Full: shell, system applets and the applications tested with full support.
        { L"BWM.EXE", kModeFull },
        { L"cleanmgr.exe", kModeFull },
        { L"colorcpl.exe", kModeFull },
        { L"CV_64.exe", kModeFull },
        { L"DataPicker.exe", kModeFull },
        { L"dfrgui.exe", kModeFull },
        { L"dllhost.exe", kModeFull },
        { L"explorer.exe", kModeFull },
        { L"hwinfo64.exe", kModeFull },
        { L"iscsicpl.exe", kModeFull },
        { L"MenuBarNT_x64.exe", kModeFull },
        { L"mmc.exe", kModeFull },
        { L"msconfig.exe", kModeFull },
        { L"msinfo32.exe", kModeFull },
        { L"mstsc.exe", kModeFull },
        { L"odbcad32.exe", kModeFull },
        { L"perfmon.exe", kModeFull },
        { L"Rainmeter.exe", kModeFull },
        { L"RecoveryDrive.exe", kModeFull },
        { L"regedit.exe", kModeFull },
        { L"Restorator.exe", kModeFull },
        { L"rundll32.exe", kModeFull },
        { L"samplecontrols_x64.exe", kModeFull },
        { L"samplecontrols_x86.exe", kModeFull },
        { L"SampleControlsNT_x64.exe", kModeFull },
        { L"SearchHost.exe", kModeFull },
        { L"ShellExperienceHost.exe", kModeFull },
        { L"ShellHost.exe", kModeFull },
        { L"StartMenuExperienceHost.exe", kModeFull },
        { L"SystemPropertiesAdvanced.exe", kModeFull },
        { L"TreeView.exe", kModeFull },
        { L"unlocker.exe", kModeFull },
        { L"WinRAR.exe", kModeFull },
        { L"winver.exe", kModeFull },
    };

    // Default value of the BlockList key: a human-readable note only.
    inline constexpr const wchar_t* kBlockListDescription =
        L"BlackWindowManager' s module won't be loaded into one of these process";

    // Each entry is written as REG_DWORD 1.
    inline constexpr const wchar_t* kBlockList[] = {
        L"cdb.exe",
        L"claude.exe",
        L"copilot-language-server.exe",
        L"cursor.exe",
        L"dataexchangehost.exe",
        L"dbgx.shell.exe",
        L"LogViewer.exe",
        L"microsoft.servicehub.controller.exe",
        L"msvsmon.exe",
        L"nvcontainer.exe",
        L"nvidia overlay.exe",
        L"olk.exe",
        L"pad.browsernativemessagehost.exe",
        L"powershell.exe",
        L"reliccoh2.exe",
        L"servicehub.datawarehousehost.exe",
        L"servicehub.host.dotnet.x64.exe",
        L"servicehub.identityhost.exe",
        L"servicehub.indexingservice.exe",
        L"servicehub.testwindowstorehost.exe",
        L"useroobebroker.exe",
    };

    inline constexpr unsigned kAttachListCount = static_cast<unsigned>(sizeof(kAttachList) / sizeof(kAttachList[0]));
    inline constexpr unsigned kBlockListCount = static_cast<unsigned>(sizeof(kBlockList) / sizeof(kBlockList[0]));
}
