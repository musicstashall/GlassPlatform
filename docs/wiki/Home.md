# GlassPlatform · техническая документация

**GlassPlatform** — внутрипроцессная система визуальной модернизации Windows 10/11
(DWM Blur / Acrylic / Mica / Aero Glass). 13 модулей решения, двойная x86/x64
архитектура, без статической линковки между peer-DLL: межмодульные вызовы идут
через явные C-экспорты, `GetModuleHandleW`, `Shared::GlassStop` и разделяемую память ядра.

| | |
|---|---|
| Модулей решения | **13** |
| Файлов `*Hooks.cpp` в `GlassCore` | **33** |
| Контролы / popup-семейства `WndCompat` | **21 / 4** |
| Списки attach / block / built-in | **64 / 21 / 55** |
| `InjectProcessMap::Slot` / `TrayXamlPolicy.v4` | **176 B / 108 B** |
| Диапазон `WM_APP`-сообщений | **0x200 .. 0xD00** |

## Разделы

| № | Раздел | Диаграмма |
|---|---|---|
| 01 | [Обзор 13 модулей и граф границ доверия](01-overview) | Mermaid · зоны A / B / C1 / C2 |
| 02 | [Вход в процесс, Gate R7 и CoreWindow-ветвление](02-init) | Mermaid · `flowchart` |
| 03 | [33 модуля перехвата, IATRegistry и PaintDispatcher](03-hooks-pipeline) | Mermaid · `flowchart` |
| 04 | [Конвейер 32bpp, свечение текста и капсула GVT1](04-gdi-32bpp) | Mermaid · `flowchart` |
| 05 | [`GlassAttachGate`, 6 ролей `WindowClassifier`, 21 + 4 обработчика](05-window-classifier) | Mermaid · `flowchart` |
| 06 | [Explorer WinUI 3 IXDE (Zone 3/4) и 6 поверхностей TaskbarCompat](06-win11-xaml) | Mermaid · стек слоёв |
| 07 | [UICompat: AppContainer, PLM и `TrayXamlPolicy`](07-appcontainer) | Mermaid · зоны доверия |
| 08 | [Двухуровневый IPC, объекты ядра и бинарные секции](08-ipc) | Таблица разметки 176 B |
| 09 | [Иерархия реестра, резолвер политик, `EffectType 0–8`](09-registry) | Mermaid · каскад |
| 10 | [Детерминированная выгрузка `ExecuteShutdown` (S0 + 1–6)](10-unload) | Mermaid · `sequenceDiagram` |
| 11 | [GlassShellExt, LogViewer и GlassSettings](11-tools) | — |
| 12 | [Сборка, 5 MSI-пакетов установщика и подпись](12-build-signing) | — |
| 13 | [Текстуры стекла OpenGlass и ReflectionTextureService](13-openglass-textures) | Mermaid · `flowchart` |

## Где что лежит: сайт и GitHub

Документация живёт на двух площадках, и это не дубли, а разные режимы чтения.

| Документ | На сайте | На GitHub |
|---|---|---|
| Техническая документация, 13 разделов | [docs.html](https://glass-platform.ru/docs.html) — интерактивные схемы, фильтры по веткам и ролям, симуляторы | [эта вики](Home) — те же разделы текстом и диаграммами Mermaid |
| Параметры реестра `HKCU\Software\StanislavKa\GlassPlatform` | [registry.html](https://glass-platform.ru/registry.html) — поиск по всем параметрам, сводные таблицы перекрытий, переключатели разделов | [`docs/registry.md`](https://github.com/musicstashall/GlassPlatform/blob/master/docs/registry.md) — markdown-исходник справочника, история правок в git |
| Подпись сборок и состав подписи | отдельной страницы нет | [`docs/signing.md`](https://github.com/musicstashall/GlassPlatform/blob/master/docs/signing.md) |
| Инструкции по установке и настройке | [help.html](https://glass-platform.ru/help.html) | — |

**Какую ссылку давать.** Нужно найти параметр или показать схему — сайт. Нужна история
изменений, `diff`, ссылка из кода, правка через PR или точная формулировка, которую можно
процитировать в issue, — файл в репозитории. Если две версии разошлись, ориентируйтесь на
файл: `registry.html` сам указывает `docs/registry.md` как источник.

## Интерактивная версия

Часть схем на сайте интерактивна (клик по узлу, фильтры по ролям, симуляторы
`IATRegistry` и углов панели задач). В вики этого нет — GitHub вырезает скрипты,
стили и `iframe`. Полные версии:

- [Техническая документация · docs.html](https://glass-platform.ru/docs.html)
- [Справочник параметров реестра](https://glass-platform.ru/registry.html)
- [Инструкции](https://glass-platform.ru/help.html)

Исходники страниц этой вики лежат в репозитории: `docs/wiki/`.
