# AGENTS — GlassProject

Язык ответов: **русский**.

## Порядок чтения

1. [CLAUDE.md](CLAUDE.md) — правила, anti-patterns, workflow (локально; зеркало путей ниже)
2. [ARCHITECTURE.md](ARCHITECTURE.md) — процессы, mutex/events, InjectMap, линковка peer DLL
3. Индекс модулей — README в каталоге проекта (см. [Модули](#модули))
4. Выгрузка GlassCompat: [GlassCompat/docs/GlassCompat-unload.md](GlassCompat/docs/GlassCompat-unload.md) — **не ломать** path=1 vs BlockList

**GlassSettings (detail-страницы, транзакции UI+реестр+notice):** [GlassSettings/docs/GlassSettings-page-pipeline.md](GlassSettings/docs/GlassSettings-page-pipeline.md) — serial-конвейер `RunSequenceAsync`, порядок фаз Animating → RegistryMutation → Cosmetic → NoticeSync.

## Профиль (кратко)

| Параметр | Значение |
|----------|----------|
| Приоритет разработки | 1) GlassCore, 2) WndCompat |
| Замороженные зоны | `ExplorerCompat/docs/frozen/explorer-xaml/*` — только чтение без явного запроса |
| Частые задачи | новый/правка `*Hooks.cpp` |
| План перед кодом | всегда, кроме явных мелочей |
| Backup | `_backups/<YYYY-MM-DD_HHMM>/` |
| Сборка | x64 Debug из корня solution → `Build\x64\Debug\` |

## Модули

| Модуль | README | Ключевые гайды |
|--------|--------|----------------|
| GlassCompat | [GlassCompat/README.md](GlassCompat/README.md) | [unload](GlassCompat/docs/GlassCompat-unload.md) |
| GlassCore | [GlassCore/README.md](GlassCore/README.md) | [hooks-and-paint](GlassCore/docs/hooks-and-paint.md) |
| WndCompat | [WndCompat/README.md](WndCompat/README.md) | — |
| ExplorerCompat | [ExplorerCompat/README.md](ExplorerCompat/README.md) | [ExplorerXamlIslands](ExplorerCompat/docs/ExplorerXamlIslands.md) |
| TaskbarCompat | [TaskbarCompat/README.md](TaskbarCompat/README.md) | [TaskbarBorders](TaskbarCompat/docs/TaskbarBorders.md) |
| UICompat | [UICompat/README.md](UICompat/README.md) | [UICompat-CoreWindow](UICompat/docs/UICompat-CoreWindow.md) |
| GlassSettings | [GlassSettings/README.md](GlassSettings/README.md) | [WinUI](GlassSettings/docs/GlassSettings-WinUI.md), [page-pipeline](GlassSettings/docs/GlassSettings-page-pipeline.md) |
| GlassSetup | [GlassSetup/README.md](GlassSetup/README.md) | [GlassSetup/docs/](GlassSetup/docs/) |
| GlassShared | [GlassShared/README.md](GlassShared/README.md) | [gdi-alpha](GlassShared/docs/gdi-alpha.md) |
| LogViewer | [LogViewer/README.md](LogViewer/README.md) | — |
| GlassShellExt | [GlassShellExt/README.md](GlassShellExt/README.md) | — |

## Сборка

Используй скилл **build** (`.cursor/skills/build/SKILL.md`). Сборка только из корня solution. **Не** запускать/останавливать платформу; при LNK1168 — вопрос пользователю и ждать ответа ([contributing/build.md](contributing/build.md)).

## Кодировка

После создания файла — проверить UTF-8 (не UTF-16). Исправлять сразу, пока файл пустой/короткий: [contributing/file-encoding.md](contributing/file-encoding.md).

## Порядок в репозитории

Разовые скрипты — только `.claude/tmp/`, удалять после задачи. Не создавать `_tmp*.py` в корне. См. [contributing/repo-hygiene.md](contributing/repo-hygiene.md).

## Не трогать без явного запроса

- `ExplorerCompat/docs/frozen/**`

## Запрещённые решения (не предлагать повторно)

- **Пункт контекстного меню рабочего стола — только регистрацией DLL.** Классическое Win32-меню (`IContextMenu`) создаётся/снимается исключительно `DllRegisterServer`/`DllUnregisterServer` `GlassShellExt.dll` через `regsvr32 /s` / `regsvr32 /u /s` (Win10 всегда; Win11 — только fallback, когда sparse-пакет зарегистрировать нельзя). Компактное меню Win11 — только sparse-пакет (`AppxManifest.xml`). Записывать вручную ключи `…\DesktopBackground\shellex\ContextMenuHandlers\…`, `CLSID\…\InprocServer32`, `ExplorerCommandHandler` и подобные из GlassSettings.Native, Setup, GlassUnins или скриптов **запрещено** — такие предложения отклоняются. Подробности: [GlassShellExt/README.md](GlassShellExt/README.md), проверка — `GlassSetup/tests/verify_resources.py`.
- **Сертификат разработчика — только по явному выбору пользователя.** Единственный код, добавляющий его в `LocalMachine\TrustedPeople`, — `ShellSparsePackage::TrustSparseSigner`; вызывается со страницы «Сертификат разработчика» установщика или по ссылке в GlassSettings → Система. Сборка ничего не создаёт/не экспортирует (`New-SelfSignedCertificate`, `Import-Certificate` и т. п. запрещены), GlassUnins сертификат не удаляет. Пользователю не предлагать ручные шаги (regedit, certlm, PowerShell) там, где есть кнопка.
- **Единственная папка платформы в профиле — `%LOCALAPPDATA%\GlassPlatform`** (логи — `Logs\`, дампы — `Crashes\`). `AppData\Local\GlassProject` и другие каталоги не создавать.
- **SecureUxTheme — отдельный per-machine MSI** (System32/SysWOW64, IFEO/COM через custom action пакета). GlassSettings и GlassUnins перед удалением только переключают штатный стиль текущего пользователя (`GlassSettings_SecureUxThemeSetupV1W`); снятие hook и файлов — удалением MSI. Тумблер в GlassSettings ставит/снимает MSI из `%ProgramData%\Packages\GlassPlatform\Current`.
- **Политика автозапуска OpenGlass в установщике — `!filesPresent || wasRunning`** (решение пользователя 2026-11-03, «оставить текущее правило»): запускать после установки только при свежей установке или если OpenGlass работал раньше; существующую установку «файлы без оркестрации» не автоматизировать. Предложения «всегда запускать» отклоняются.

## Скиллы проекта

`glass-workflow`, `glass-hooks`, `glass-bwm-port`, `glass-wndcompat`, `glass-explorer-xaml`, `crashdump` — см. `.cursor/skills/`.

При любом падении/дампе (`.dmp`, WER, `CrashDumps`, FailFast, CFG) — сначала скилл `crashdump`.
