# GlassShellExt

**Связанный код:** `GlassShellExt/`, `GlassSettings/Native/ShellExtensionOps.cpp`

**GlassShellExt** (`GlassShellExt\`): контекстное меню по пустому фону рабочего стола. Explorer загружает `GlassShellExt.dll` по COM при открытии меню.

Раскладка: `GlassShellExt.dll`, `GlassShellHost.exe`, `GlassShellSparse.msix`, `AppxManifest*.xml`, `Assets\` лежат в подкаталоге платформы **`GlassShellSparse\`** — и в сборке (`Build\<arch>\<cfg>\GlassShellSparse\` = OutDir обоих проектов), и после установки (`<root>\GlassShellSparse\`). Он же — ExternalLocation sparse-пакета. Пути — только через `GlassShared\include\GlassShellMenuConstants.h` (`kSparsePackageSubdir`, `kShellExtRelativePath`, `kShellHostRelativePath`); `GlassSettings.exe` ищется из DLL как `<родитель каталога DLL>\GlassSettings\` (`DesktopShellActions.cpp`).

## Регистрация по ОС

| ОС | Способ | Меню | HKCU `ContextMenuHandlers` |
|----|--------|------|----------------------------|
| **Win11** (build ≥ 22000), сертификат разработчика доверен | sparse package `GlassProject.GlassShellMenu_83517z3ecdax2` (`RegisterSparsePackageQuiet`, `AppxManifest.xml`, ExternalLocation `<root>\GlassShellSparse\`) | `IExplorerCommand` (compact-меню) | **не должны быть** (после успеха sparse `RemoveClassicAfterSparseSuccess` снимает классическую регистрацию через `regsvr32 /u /s`) |
| **Win11**, сертификат **не** доверен (sparse не регистрируется, `0x800B0109`) | классический fallback: `regsvr32 /s GlassShellExt.dll` → `DllRegisterServer` (`RegisterClassicViaRegsvr32`), `GlassSettings_GetShellMenuModeV1() == 1` | `IContextMenu` после «Показать дополнительные параметры» (Shift+F10) | **штатная схема** — пишет только `DllRegisterServer` |
| **Win10** (build < 22000) | `regsvr32 /s GlassShellExt.dll` → `DllRegisterServer` | `IContextMenu` (`DesktopBackground`) | **штатная схема** — привязка shell к CLSID `{A91C9F80-7E4B-4D8A-9C16-F304E8B2D601}` |

**Правило (обязательное).** Классическое Win32-меню создаётся и снимается **только самой DLL** — `DllRegisterServer`/`DllUnregisterServer` `GlassShellExt.dll`, вызываемые через `regsvr32 /s` и `regsvr32 /u /s` (`ShellExtensionOps.cpp`: `RegisterClassicViaRegsvr32`/`UnregisterClassicViaRegsvr32`, на Win10 — `RunRegsvr32`). Ни GlassSettings.Native, ни Setup, ни GlassUnins **не пишут** `HKCU\Software\Classes\...\DesktopBackground\shellex\ContextMenuHandlers\...`, `CLSID\...\InprocServer32` и подобные ключи вручную; предложения «добавить пункт меню записью в `shellex`» отклоняются без обсуждения. Sparse (compact) и классическая регистрация взаимно исключены: после успешного sparse классическая снимается, при fallback sparse отсутствует. Проверка — `GlassSetup\tests\verify_resources.py`.

На Win11 `DllRegisterServer` пишет те же классические ключи, что и на Win10 (compact-меню объявлено только в манифесте sparse-пакета: `com:ComServer`, `FileExplorerContextMenus`; `ExplorerCommandHandler` в HKCU DLL не пишет — иначе пункты дублируются). На Win10 compact XAML-меню нет — sparse не используется.

Оркестрация: `GlassSettings/Native/ShellExtensionOps.cpp` (ветка по `build >= 22000`): sparse → при неудаче (кроме конфликта внешнего каталога `0x47504346`) классический fallback через `regsvr32`; состояние для UI — `GlassSettings_GetShellMenuModeV1` (0 нет / 1 classic / 2 compact) и `GlassSettings_GetShellSignerTrustV1` (`ShellSparsePackage::QuerySparseSignerTrust`: подписант msix в «Доверенных лицах» CurrentUser/LocalMachine; только чтение). Помощник установщика возвращает `0x47504C00|причина` при fallback.

## Сертификат разработчика («Доверенные лица»)

Единственное место, где сертификат разработчика попадает в хранилище, — `ShellSparsePackage::TrustSparseSigner` (`LocalMachine\TrustedPeople`, `CertAddCertificateContextToStore`, Subject обязан совпадать с `kSparsePublisher`): сертификат берётся из подписи установленного `GlassShellSparse.msix`, ничего не создаётся и не экспортируется. Вызывается **только по явному выбору пользователя**:

- **Setup** (Win11 x64, если подписант собственного `GlassSetup.exe` ещё не доверен) — отдельная страница «Сертификат разработчика» (что это, зачем, Subject/отпечаток/срок/хранилище, два варианта; по умолчанию «установить»); после установки файлов — `rundll32 GlassSettings.Native.dll,GlassSettings_ShellSignerTrustSetupV1W /installroot "<root>"` (код `0x47535400|результат` при ошибке → заметка на финальной странице, без отката), затем регистрация меню. Отказ — классическое меню и заметка со ссылкой на `DeveloperCodeSigning.cer`/`.txt`. EULA (раздел 5) описывает сертификат и право отказаться.
- **GlassSettings** (страница «Система», уведомление при `Untrusted`) — ссылка «Установить сертификат разработчика и включить компактное меню»: `GlassSettings_TrustShellSignerV1` in-process (процесс elevated) → перерегистрация пункта (sparse, классическая снимается). При `Trusted`+classic — ссылка «Перейти на компактное меню».
- **GlassUnins никогда не удаляет сертификат** — удаление только вручную (`certlm.msc` → «Доверенные лица»), это описано в EULA и `DeveloperCodeSigning.txt`.


## Compact-меню (Win11)

Один root-verb (CLSID toggle `{6D4E18F4-...}`) с `ECF_HASSUBCOMMANDS`; вложенные пункты «start/stop» и «настройки» — через `EnumSubCommands` (класс config `{8B219A33-...}`). Иконки: `GlassShellExt\Resources\` (`app.ico`, `pt.ico`, `st.ico`), `GetIcon` → `"<path_to_GlassShellExt.dll>,-<id>"`. Строки: `GlassShellExt\resource.rc` (en/ru/de/fr/zh).

## Sparse package

`Application@Executable` = `GlassShellHost.exe` (package host, external location); COM/меню — `GlassShellExt.dll`. Deploy/detect/unregister — `GlassSettings::ShellSparsePackage` (WinRT `AddPackageByUriAsync` msix + `ExternalLocationUri`, fallback `RegisterPackageByUriAsync` по loose-манифесту / `RemovePackageAsync`).

Идентичность: `Identity@Publisher` = `CN=Stanislav K.` = Subject сертификата разработчика (`Shared\certs\DeveloperCodeSigning.cer`, pin в `Shared/GlassSign.targets`) — требование MSIX; семейство `GlassProject.GlassShellMenu_83517z3ecdax2` (13 символов = Crockford-base32 первых 8 байт SHA-256 от Publisher в UTF-16LE; проверка — `GlassSetup\tests\sparse_identity_mock.py`, на Windows `Get-AppxPackage GlassProject.GlassShellMenu`). Сборка: `Package\BuildSparsePackage.ps1` (PostBuild) собирает `GlassShellSparse.msix` только `makeappx.exe`; подпись — общий `GlassSignPaths` (`GlassShellExtSignPaths` в `.vcxproj`), т.е. без ключа автора msix остаётся неподписанным (сборка контрибьютора — классический fallback на Win11). Никаких `New-SelfSignedCertificate`/`TrustedPeople` в сборке нет. Смена сертификата = смена Publisher и семейства пакета (старые регистрации не мигрируются).

## Старт / стоп платформы

Пункты «Включить/Остановить платформу» управляют **только задачами** планировщика (`GlassPlatformTask::StartPlatform` / `SignalPlatformStop`). GlassShellExt **не** резолвит пути `GlassCompat.dll` и не запускает `rundll32` напрямую. Нет задачи — пункт disabled. Создание задачи — только из GlassSettings (после установки соответствующей битности).

## Запуск GlassSettings из меню

Оба пути (`IExplorerCommand` / `IContextMenu`) вызывают `LaunchGlassSettings` в `DesktopShellActions.cpp`. Перед запуском — `TryActivateExistingGlassSettings`: top-level HWND `GlassSettings.exe` (EnumWindows: visible / iconic / hidden fallback, без owner/toolwindow) или mutex `GlassSettings.Instance.Mutex.x64` / `.x86` (как в `SingleInstance.cs`). При успехе — без нового процесса (иначе лишний UAC из‑за `requireAdministrator`): `AllowSetForegroundWindow` + `PostMessage(WM_GLASS_SETTINGS_ACTIVATE)` → MainWindow in-process `OverlappedPresenter.Restore` / `AppWindow.Show` / `Activate`. GS разрешает это сообщение через `ChangeWindowMessageFilterEx` (UIPI: medium explorer → elevated GS иначе `PostMessage` → FALSE). Внешний Win32 `ShowWindow` для WinUI AppWindow часто no-op — только fallback. Запуск нового — через `explorer.exe` (trampoline): прямой `ShellExecute` на `GlassSettings.exe` из sparse package наследует identity `GlassShellMenu` и ломает WinUI; `explorer.exe "…\GlassSettings.exe"` стартует unpackaged (как из LogViewer). Диагностика — `%LOCALAPPDATA%\GlassPlatform\Logs\GlassShellExt-launch.log` (единственный пользовательский каталог платформы — `GlassPlatform`; `GlassProject` в AppData не создаётся).
