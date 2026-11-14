# GlassSettings

**Связанный код:** `GlassSettings/` (WinUI, C#), `GlassSettings/Native/` (C++ helper, unlocker, presence).

Приложение параметров платформы. Detail-страницы используют serial-конвейер UI + реестр + notice — **обязательно** [docs/GlassSettings-page-pipeline.md](docs/GlassSettings-page-pipeline.md) перед правками страниц.

## Версия Settings

Основная версия приложения и обоих MSI Settings берётся из общего источника
комплекта: `GlassProductVersion` в корневом `Directory.Build.props` и номер
комплекта текущей сессии. Отдельной версии Settings нет. FileVersion сохраняет
собственный счётчик проекта, его первые три поля — те же `2.0.0`. Native helper
нумеруется отдельно и не задаёт версию приложения.

ProductVersion EXE — версия комплекта. Перед упаковкой MSI read-only проверка
сверяет её с ProductVersion пакета; устаревший EXE или чужая подмена версии
прерывают сборку. EXE и Native при этом не запускаются. Single-file publish
поддерживается: managed DLL для проверки не нужна.

Оба payload Settings собираются в сессии комплекта: **Release|x64: GlassShared →
GlassSettings (сборка Native выполняется его target) → GlassSettings-x64**; затем
**Release|x86: GlassShared → GlassSettings → GlassSettings-x86**. Нужен ранее
собранный Release x86 GlassUnins; при его отсутствии сначала собрать
**Release|x86: GlassShared → GlassUnins**. После пересборки продуктов каталог
релиза создаётся заново из готовых MSI. См.
[контракт версий](../GlassSetup/docs/msi-product-versions.md).

## Документация

| Задача | Документ |
|--------|----------|
| Stock theme перед удалением SecureUxTheme | [docs/stock-theme-maintenance.md](docs/stock-theme-maintenance.md) |
| Обзор WinUI | [docs/GlassSettings-WinUI.md](docs/GlassSettings-WinUI.md) |
| Page operation pipeline | [docs/GlassSettings-page-pipeline.md](docs/GlassSettings-page-pipeline.md) |
| Notice / defer / external-sync | [docs/GlassSettings-notices.md](docs/GlassSettings-notices.md) |
| Страница «Композиция» (эталон) | [docs/DwmColorizationPage.md](docs/DwmColorizationPage.md) |
| Host-glass / live reload | [docs/GlassSettings-host-glass.md](docs/GlassSettings-host-glass.md) |
| Live OFF Root backdrop (NONE) | [docs/RootBackdropDisableSequence.md](docs/RootBackdropDisableSequence.md) |
| PlatformPresenceService | [docs/PlatformPresenceService-architecture.md](docs/PlatformPresenceService-architecture.md) |
| Разработка UI | [docs/GlassSettings-development.md](docs/GlassSettings-development.md) |
| Hosted-диалоги и штора | [docs/GlassSettings-hosted-dialog.md](docs/GlassSettings-hosted-dialog.md) |
| Слайдеры (deferred-commit) | [docs/GlassSettings-slider-commit.md](docs/GlassSettings-slider-commit.md) |
| NumberBox (commit по Enter) | [docs/GlassSettings-numberbox-commit.md](docs/GlassSettings-numberbox-commit.md) |
| Локализация | [docs/GlassSettings-localization.md](docs/GlassSettings-localization.md) |
| Detail-карточки | [docs/GlassSettings-detail-cards.md](docs/GlassSettings-detail-cards.md) |
| Customization / baseline | [docs/GlassSettings-customization-tracking.md](docs/GlassSettings-customization-tracking.md) |
| Тултипы (acrylic) | [docs/GlassSettings-tooltips.md](docs/GlassSettings-tooltips.md) |
| Миниатюры атласов | [docs/GlassSettings-atlas-preview.md](docs/GlassSettings-atlas-preview.md) |
| Crossfade обоев в превью | [docs/GlassSettings-wallpaper-preview-crossfade.md](docs/GlassSettings-wallpaper-preview-crossfade.md) |
| Превью «Стилизация» | [docs/GlassSettings-stylization-layered-preview.md](docs/GlassSettings-stylization-layered-preview.md) |
| Adaptive layout | [docs/GlassSettings-adaptive-layout.md](docs/GlassSettings-adaptive-layout.md) |
| Visual styles | [docs/GlassSettings-visual-styles.md](docs/GlassSettings-visual-styles.md) |
| Startup white flash | [docs/GlassSettings-startup-white-flash.md](docs/GlassSettings-startup-white-flash.md) |
| Мастер ink (TextVisualTuning) | [docs/TextVisualTuningWizard.md](docs/TextVisualTuningWizard.md) |

## Список процессов и остановка платформы

- **Источник данных — карта инжекта и скан процессов** (`Services/InjectRuntimeMonitor.cs`),
  без собственного кэша отвергнутых записей: карта закрылась — список очистился.
- **Собственные инструменты не показываются:** `GlassSettings.exe` и `LogViewer.exe`
  отфильтрованы во всех ветках сборки списка (`IsOwnToolProcess`). Хост платформы
  (`rundll32` + `GlassCompat`) видим — он и есть предмет наблюдения, но в счётчик остатков
  не входит.
- **Выгрузка показывается постепенно:** строка не исчезает мгновенно, индикаторы модулей
  гаснут по одному (`kFadeTicks`). Догорающие строки не считаются остатком.
- **«Отвергнутые»** — процессы, которым гейт GlassCompat отказал в загрузке (бит
  `ModuleCompat` в слоте карты).
- **Остановка** идёт через общий координатор: `NativeHostInterop.RequestPlatformStop` →
  `Shared::GlassStop` (`Reason::User`), после чего UI показывает остатки, если они есть.
  См. [GlassShared/docs/stop-unification.md](../GlassShared/docs/stop-unification.md).

См. также: [GlassCore/docs/glasscore-text.md](../GlassCore/docs/glasscore-text.md) (рендер текста / ink).


## SecureUxTheme: роль Settings

Переключатель управляет регистрацией IFEO/COM, а не MSI. Отключение возвращает штатные COM InProcServer32 и убирает токены загрузки SecureUxTheme4.dll; файлы, MSI и его запись удаления сохраняются. Перед отключением активного механизма проверяется штатный visual style, после изменения регистрации нужна перезагрузка для уже загруженных хуков. MSI устанавливает GlassSetup, удаляет Windows Installer/деинсталлятор.

После штатного удаления авторского MSI могут остаться permanent ThemeUiProxy и COM-перенаправления. Неактивные файлы — не конфликт и не признак установленного продукта. Оставшуюся COM-регистрацию можно отключить без патчера; включение требует полного установленного набора native/WOW модулей. Чужие COM значения и активные несовместимые старые hooks не перезаписываются. Подробности и незавершённое обслуживание: [GlassSetup README](../GlassSetup/README.md).

## Архив базы параметров (System → Резервные копии параметров)

«Изменить» в «Обслуживании систем» запускает проверенный cached Setup через maintenance ticket.
«Создать копию» создаёт уникальный `GlassSettingsBackup-*.glassbackup`
в корне Documents. Внутри это zip: `manifest.json` (formatVersion=2, список частей,
SHA256) и `registry.json` — массив Section/Hive/View/Key/Name/Type/Data.
Data — Base64 исходных байтов; Type — числовой Win32 REG_*. Type/Data=null у имени
OpenGlass означает, что значения не было, и восстановление его удаляет.

Копия только HKCU текущего пользователя, обе view на 64-bit. Чужие профили и HKLM
не читаются. Пути установки, `Crashes` и `Installer` не входят. Части:

- оформление Glass — корень `Software\StanislavKa\GlassPlatform` и подключи кроме списков, `GlassSettings`, `Crashes`, `Installer`;
- списки процессов — только `AttachList` и `BlockList`;
- OpenGlass — 49 имён из `kDwmOpenGlassRules`, не пять штатных цветов Windows и не весь раздел DWM;
- параметры приложения — подключ `GlassSettings`.

Восстановление пишет выбранные части обратно в HKCU и рассылает live-reload.
Файл не зашифрован. Существующие файлы не перезаписываются. Двойной щелчок по
`.glassbackup` открывает GlassSettings на странице «Система», вкладке «Восстановить».

Реализация: `Services/SettingsRegistryBackupService.cs`, UI SystemPage.
Source contract: `GlassSetup/tests/settings_backup_contract.py`. Это не замена
C# compilation / Windows VM tests файловых и registry API.
