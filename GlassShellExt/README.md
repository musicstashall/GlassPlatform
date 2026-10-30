# GlassShellExt

**Связанный код:** `GlassShellExt/`, `GlassSettings/Native/ShellExtensionOps.cpp`

**GlassShellExt** (`GlassShellExt\`): контекстное меню по пустому фону рабочего стола. Explorer загружает `GlassShellExt.dll` по COM при открытии меню.

## Регистрация по ОС

| ОС | Способ | Меню | HKCU `ContextMenuHandlers` |
|----|--------|------|----------------------------|
| **Win11** (build ≥ 22000) | sparse package `GlassProject.GlassShellMenu` (`RegisterSparsePackageQuiet`, `AppxManifest.xml`, external location) | `IExplorerCommand` (compact-меню) | **не должны быть** (миграционный мусор от старого `regsvr32`) |
| **Win10** (build < 22000) | `regsvr32 GlassShellExt.dll` → `DllRegisterServer` | `IContextMenu` (`DesktopBackground`) | **штатная схема** — привязка shell к CLSID `{A91C9F80-7E4B-4D8A-9C16-F304E8B2D601}` |

На Win11 `regsvr32` и `DllRegisterServer` — no-op: COM и verbs объявлены в манифесте (`com:ComServer`, `FileExplorerContextMenus`). На Win10 compact XAML-меню нет — sparse не используется.

Оркестрация: `GlassSettings/Native/ShellExtensionOps.cpp` (ветка по `build >= 22000`).

## Compact-меню (Win11)

Один root-verb (CLSID toggle `{6D4E18F4-...}`) с `ECF_HASSUBCOMMANDS`; вложенные пункты «start/stop» и «настройки» — через `EnumSubCommands` (класс config `{8B219A33-...}`). Иконки: `GlassShellExt\Resources\` (`app.ico`, `pt.ico`, `st.ico`), `GetIcon` → `"<path_to_GlassShellExt.dll>,-<id>"`. Строки: `GlassShellExt\resource.rc` (en/ru/de/fr/zh).

## Sparse package

`Application@Executable` = `GlassShellHost.exe` (package host, external location); COM/меню — `GlassShellExt.dll`. Deploy/detect/unregister — `GlassSettings::ShellSparsePackage` (WinRT `RegisterPackageByUriAsync` / `RemovePackageAsync`).

## Старт / стоп платформы

Пункты «Включить/Остановить платформу» управляют **только задачами** планировщика (`GlassPlatformTask::StartPlatform` / `SignalPlatformStop`). GlassShellExt **не** резолвит пути `GlassCompat.dll` и не запускает `rundll32` напрямую. Нет задачи — пункт disabled. Создание задачи — только из GlassSettings (после установки соответствующей битности).

## Запуск GlassSettings из меню

Оба пути (`IExplorerCommand` / `IContextMenu`) вызывают `LaunchGlassSettings` в `DesktopShellActions.cpp`. Перед запуском — `TryActivateExistingGlassSettings`: top-level HWND `GlassSettings.exe` (EnumWindows: visible / iconic / hidden fallback, без owner/toolwindow) или mutex `GlassSettings.Instance.Mutex.x64` / `.x86` (как в `SingleInstance.cs`). При успехе — без нового процесса (иначе лишний UAC из‑за `requireAdministrator`): `AllowSetForegroundWindow` + `PostMessage(WM_GLASS_SETTINGS_ACTIVATE)` → MainWindow in-process `OverlappedPresenter.Restore` / `AppWindow.Show` / `Activate`. GS разрешает это сообщение через `ChangeWindowMessageFilterEx` (UIPI: medium explorer → elevated GS иначе `PostMessage` → FALSE). Внешний Win32 `ShowWindow` для WinUI AppWindow часто no-op — только fallback. Запуск нового — через `explorer.exe` (trampoline): прямой `ShellExecute` на `GlassSettings.exe` из sparse package наследует identity `GlassShellMenu` и ломает WinUI; `explorer.exe "…\GlassSettings.exe"` стартует unpackaged (как из LogViewer). Диагностика — `%LOCALAPPDATA%\GlassProject\GlassShellExt-launch.log`.
