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

## Скиллы проекта

`glass-workflow`, `glass-hooks`, `glass-bwm-port`, `glass-wndcompat`, `glass-explorer-xaml`, `crashdump` — см. `.cursor/skills/`.

При любом падении/дампе (`.dmp`, WER, `CrashDumps`, FailFast, CFG) — сначала скилл `crashdump`.
