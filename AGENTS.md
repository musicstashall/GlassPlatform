# AGENTS — GlassProject

Язык ответов: **русский**.

## Облачная ветка Arena

В Arena резервные копии `_backups/` и одноразовые HTML-макеты не создавать: изменения сохраняются в отдельной Git-ветке. Локальные требования backup ниже относятся к локальным агентам. Не удалять рабочие HTML-ресурсы приложения. **После каждой согласованной правки, перед тестом со стороны пользователя, обязательны коммит и push в текущую ветку без дополнительного согласования; визуальной приёмки для публикации не ждать.** В отчёте указывать хэш, результат push и незавершённые проверки. Не останавливаться в ожидании подтверждения без конкретного вопроса.

После анализа загруженных пользователем файлов журналов сохранять необходимые выводы и удалять исходные файлы логов из каталога входящих вложений. Не накапливать их копии в репозитории. Изображения и другие вложения не считать логами и автоматически по этому правилу не удалять.

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
| GlassSetup | [GlassSetup/README.md](GlassSetup/README.md) | [публикация](GlassSetup/docs/release-publish.md), [docs](GlassSetup/docs/) |
| GlassShared | [GlassShared/README.md](GlassShared/README.md) | [gdi-alpha](GlassShared/docs/gdi-alpha.md) |
| LogViewer | [LogViewer/README.md](LogViewer/README.md) | — |
| GlassShellExt | [GlassShellExt/README.md](GlassShellExt/README.md) | — |

## Сборка

Используй скилл **build** (`.cursor/skills/build/SKILL.md`). Сборка только из корня solution. **Не** запускать/останавливать платформу; при LNK1168 — вопрос пользователю и ждать ответа ([contributing/build.md](contributing/build.md)).

Публикация комплекта и отдельных MSI — скилл **glass-release** и [GlassSetup/docs/release-publish.md](GlassSetup/docs/release-publish.md). Новую сборку ради публикации не начинать; `version.json` заливать последним.

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
- **Политика автозапуска OpenGlass в установщике — `autostartAllowed && valid`** (решение пользователя 2026-11-27, отменяет правило 2026-11-03 `!filesPresent || wasRunning`): выбранный OpenGlass запускается и после установки, и после обновления/переустановки, даже если он не был запущен при открытии мастера. Страховка от циклических падений DWM (26H2/26300.9818) — сторож `OpenGlassDwmGuard`, а не запрет запуска. Предыдущее правило отменено явно; возвращающие его предложения отклоняются без нового явного запроса.

## Сторож падений DWM (OpenGlassDwmGuard) — сводка для агентов

Принято 2026-11-27 (согласованный план). Реализация: `GlassShared/{include/OpenGlassDwmGuard{,Policy}.h, src/OpenGlassDwmGuard.cpp}`; мосты: `GlassSetup` (arm в `LaunchSelectedProducts` до старта OG + в `EnsureOpenGlassBeforePlatform`; подавление по маркеру; ожидание ≤33 с; finish warning) и `GlassSettings` (`Native/OpenGlassDwmGuardNative.cpp`, `Services/OpenGlassDwmGuardService.cs`, уведомления Home/System с действием «Включить автозапуск и запустить»; согласующая кнопка в `PlatformSchedulerRow`).

Семантика (не менять молча — только новым решением пользователя):
- сторож вооружается ДО старта OG; базовая идентичность dwm.exe (PID + FILETIME создания) снимается синхронно;
- второй краш dwm.exe в скользящем окне 30 с → карантин: OG останавливается, автозапуск отключается (служба → DEMAND_START; явный DISABLED не перетирается; legacy-задача → `schtasks /Change /DISABLE`);
- мгновенный краш (≤5 с) → режим Danger («самый опасный» сценарий 26H2): следующий краш → тот же карантин; одиночный — Suspect;
- абсолютный предохранитель: 3-й краш за цикл наблюдения → карантин независимо от окна;
- сторож живёт до 120 с; без единого краша выходит досрочно за 30 с; `SM_SHUTTINGDOWN` → тихий выход;
- маркер `HKCU\Software\StanislavKa\GlassPlatform\OpenGlassGuard` переживает выход из системы; карантин снимается строго более новой версией OG (IsVersionStrictlyGreater по OgVersion в маркере) либо вручную в GlassSettings;
- кросс-процессная единственность — именованный мьютекс `Local\GlassPlatform.OpenGlassDwmGuard.Watch` (watcher = процесс, запустивший OG; фонового процесса нет); смена состояния — событие `Local\GlassPlatform.OpenGlassDwmGuard.StateChanged` (GS поллит маркер по таймеру 500 мс);
- ручной запуск OG разрешён всегда; Setup не стартует OG автоматически при активном карантине;
- сторож НЕ удаляет службу/задачи/файлы, не трогает DWM-политику, не шлёт телеметрию; OG можно обновить — новая версия снимет карантин автоматически.

Известные границы: второй краш DWM может завершить сеанс до реакции сторожа; ложные срабатывания подавляются только проверкой SM_SHUTTINGDOWN; Setup следит ≤33 с (дальше — GS/маркер); Windows-сборка и VM-приёмка отдельно (g++-мок `openglass_dwm_guard_policy_mock.py` + source-gate `*_mock.py` — не доказательство на Windows).

## Скиллы проекта

`glass-workflow`, `glass-hooks`, `glass-bwm-port`, `glass-wndcompat`, `glass-explorer-xaml`, `glass-release`, `crashdump` — см. `.cursor/skills/`.

При любом падении/дампе (`.dmp`, WER, `CrashDumps`, FailFast, CFG) — сначала скилл `crashdump`.
