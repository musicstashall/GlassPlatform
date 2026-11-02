# GlassShellExt

**Связанный код:** `GlassShellExt/`, `GlassSettings/Native/ShellExtensionOps.cpp`

**GlassShellExt** (`GlassShellExt\`): контекстное меню по пустому фону рабочего стола. Explorer загружает `GlassShellExt.dll` по COM при открытии меню.

Раскладка: `GlassShellExt.dll`, `GlassShellHost.exe`, `GlassShellSparse.msix`, `AppxManifest*.xml`, `Assets\` лежат в подкаталоге платформы **`GlassShellSparse\`** — и в сборке (`Build\<arch>\<cfg>\GlassShellSparse\` = OutDir обоих проектов), и после установки (`<root>\GlassShellSparse\`). Он же — ExternalLocation sparse-пакета. Пути — только через `GlassShared\include\GlassShellMenuConstants.h` (`kSparsePackageSubdir`, `kShellExtRelativePath`, `kShellHostRelativePath`); `GlassSettings.exe` ищется из DLL как `<родитель каталога DLL>\GlassSettings\` (`DesktopShellActions.cpp`).

## Регистрация по ОС

| ОС | Способ | Меню | HKCU `ContextMenuHandlers` |
|----|--------|------|----------------------------|
| **Win11** (build ≥ 22000), сертификат автора доверен | sparse package `GlassProject.GlassShellMenu_83517z3ecdax2` (`RegisterSparsePackageQuiet`, `AppxManifest.xml`, ExternalLocation `<root>\GlassShellSparse\`) | `IExplorerCommand` (compact-меню) | **не должны быть** |
| **Win11**, сертификат **не** доверен (чужой ПК) | классический fallback: `RegisterLegacyContextMenuHandler` in-process (`GlassShellRegistration.cpp`), `GlassSettings_GetShellMenuModeV1() == 1` | `IContextMenu` после «Показать дополнительные параметры» (Shift+F10) | **штатная схема** |
| **Win10** (build < 22000) | `regsvr32 GlassShellExt.dll` → `DllRegisterServer` | `IContextMenu` (`DesktopBackground`) | **штатная схема** — привязка shell к CLSID `{A91C9F80-7E4B-4D8A-9C16-F304E8B2D601}` |

На Win11 `regsvr32` и `DllRegisterServer` — no-op: COM и verbs объявлены в манифесте (`com:ComServer`, `FileExplorerContextMenus`); классический путь использует те же общие функции `GlassShellRegistration.cpp` (`GlassShared`), что и `DllRegisterServer`. На Win10 compact XAML-меню нет — sparse не используется.

Оркестрация: `GlassSettings/Native/ShellExtensionOps.cpp` (ветка по `build >= 22000`): sparse → при неудаче (кроме конфликта внешнего каталога `0x47504346`) классический fallback; состояние для UI — `GlassSettings_GetShellMenuModeV1` (0 нет / 1 classic / 2 compact) и `GlassSettings_GetShellSignerTrustV1` (`ShellSparsePackage::QuerySparseSignerTrust`: подписант msix в «Доверенных лицах» CurrentUser/LocalMachine; только чтение). Помощник установщика возвращает `0x47504C00|причина` при fallback. GlassSettings (страница «Система») при недоверенном сертификате показывает уведомление с Subject сертификата и ссылкой на папку с `GlassAuthorCodeSigning.cer`/`.txt`; переключатель управляет классическим пунктом. **Никто не добавляет сертификат в хранилища** — импорт в «Доверенные лица» только вручную по инструкции из `.txt`.

## Compact-меню (Win11)

Один root-verb (CLSID toggle `{6D4E18F4-...}`) с `ECF_HASSUBCOMMANDS`; вложенные пункты «start/stop» и «настройки» — через `EnumSubCommands` (класс config `{8B219A33-...}`). Иконки: `GlassShellExt\Resources\` (`app.ico`, `pt.ico`, `st.ico`), `GetIcon` → `"<path_to_GlassShellExt.dll>,-<id>"`. Строки: `GlassShellExt\resource.rc` (en/ru/de/fr/zh).

## Sparse package

`Application@Executable` = `GlassShellHost.exe` (package host, external location); COM/меню — `GlassShellExt.dll`. Deploy/detect/unregister — `GlassSettings::ShellSparsePackage` (WinRT `AddPackageByUriAsync` msix + `ExternalLocationUri`, fallback `RegisterPackageByUriAsync` по loose-манифесту / `RemovePackageAsync`).

Идентичность: `Identity@Publisher` = `CN=Stanislav K.` = Subject сертификата автора (`Shared\certs\GlassAuthorCodeSigning.cer`, pin в `Shared/GlassSign.targets`) — требование MSIX; семейство `GlassProject.GlassShellMenu_83517z3ecdax2` (13 символов = Crockford-base32 первых 8 байт SHA-256 от Publisher в UTF-16LE; проверка — `GlassSetup\tests\sparse_identity_mock.py`, на Windows `Get-AppxPackage GlassProject.GlassShellMenu`). Сборка: `Package\BuildSparsePackage.ps1` (PostBuild) собирает `GlassShellSparse.msix` только `makeappx.exe`; подпись — общий `GlassSignPaths` (`GlassShellExtSignPaths` в `.vcxproj`), т.е. без ключа автора msix остаётся неподписанным (сборка контрибьютора — классический fallback на Win11). Никаких `New-SelfSignedCertificate`/`TrustedPeople` в сборке нет. Смена сертификата = смена Publisher и семейства пакета (старые регистрации не мигрируются).

## Старт / стоп платформы

Пункты «Включить/Остановить платформу» управляют **только задачами** планировщика (`GlassPlatformTask::StartPlatform` / `SignalPlatformStop`). GlassShellExt **не** резолвит пути `GlassCompat.dll` и не запускает `rundll32` напрямую. Нет задачи — пункт disabled. Создание задачи — только из GlassSettings (после установки соответствующей битности).

## Запуск GlassSettings из меню

Оба пути (`IExplorerCommand` / `IContextMenu`) вызывают `LaunchGlassSettings` в `DesktopShellActions.cpp`. Перед запуском — `TryActivateExistingGlassSettings`: top-level HWND `GlassSettings.exe` (EnumWindows: visible / iconic / hidden fallback, без owner/toolwindow) или mutex `GlassSettings.Instance.Mutex.x64` / `.x86` (как в `SingleInstance.cs`). При успехе — без нового процесса (иначе лишний UAC из‑за `requireAdministrator`): `AllowSetForegroundWindow` + `PostMessage(WM_GLASS_SETTINGS_ACTIVATE)` → MainWindow in-process `OverlappedPresenter.Restore` / `AppWindow.Show` / `Activate`. GS разрешает это сообщение через `ChangeWindowMessageFilterEx` (UIPI: medium explorer → elevated GS иначе `PostMessage` → FALSE). Внешний Win32 `ShowWindow` для WinUI AppWindow часто no-op — только fallback. Запуск нового — через `explorer.exe` (trampoline): прямой `ShellExecute` на `GlassSettings.exe` из sparse package наследует identity `GlassShellMenu` и ломает WinUI; `explorer.exe "…\GlassSettings.exe"` стартует unpackaged (как из LogViewer). Диагностика — `%LOCALAPPDATA%\GlassProject\GlassShellExt-launch.log`.
