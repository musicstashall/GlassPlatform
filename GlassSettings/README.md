# GlassSettings

**Связанный код:** `GlassSettings/` (WinUI, C#), `GlassSettings/Native/` (C++ helper, unlocker, presence).

Приложение параметров платформы. Detail-страницы используют serial-конвейер UI + реестр + notice — **обязательно** [docs/GlassSettings-page-pipeline.md](docs/GlassSettings-page-pipeline.md) перед правками страниц.

## Собственная версия Settings

`Version.props` задаёт **GlassSettingsVersion** приложения и обоих MSI Settings,
независимо от версии платформы, FileVersion Setup и версии Settings.Native.
Начальная независимая версия — **2.0.1.0**: она выше прежней `2.0.0.N` по первым
трём полям, сравниваемым Windows Installer. При новом выпуске Settings увеличивайте
эту четырёхкомпонентную release-версию; не увеличивайте её ради правок только Setup
или платформы. Если уже опубликована более высокая версия Settings, не понижайте её.

Приложение получает собственные Version/AssemblyVersion/InformationalVersion
(ProductVersion ресурса EXE). Диагностический FileVersion сохраняет per-project
build counter с префиксом версии Settings. Native helper нумеруется отдельно.
MSI packaging проверяет ProductVersion фактически упаковываемого EXE (включая
single-file publish) против release-версии и MSI; устаревший publish запрещён.
Это read-only проверка без запуска EXE и без загрузки Native/managed assembly.

Сборка обоих payload: **Release|x64: GlassShared → GlassSettings (сборка Native
выполняется его target) → GlassSettings-x64**; затем **Release|x86: GlassShared →
GlassSettings → GlassSettings-x86**. Нужен ранее собранный Release x86 GlassUnins;
при его отсутствии сначала собрать **Release|x86: GlassShared → GlassUnins**.
Пересборка платформенных MSI и Setup для этой правки не требуется.
После сборки обновить release catalog из новых MSI. См.
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

Переключатель управляет регистрацией IFEO/COM, а не MSI. Отключение возвращает штатные COM InProcServer32 и убирает токены загрузки SecureUxTheme4.dll; файлы, MSI и его запись удаления сохраняются. Перед отключением активного механизма проверяется штатный visual style, после изменения регистрации нужна перезагрузка для уже загруженных хуков. MSI устанавливает GlassSetup, удаляет Windows Installer/удалятор.

После штатного удаления авторского MSI могут остаться permanent ThemeUiProxy и COM-перенаправления. Неактивные файлы — не конфликт и не признак установленного продукта. Оставшуюся COM-регистрацию можно отключить без патчера; включение требует полного установленного набора native/WOW модулей. Чужие COM значения и активные несовместимые старые hooks не перезаписываются. Подробности и незавершённое обслуживание: [GlassSetup README](../GlassSetup/README.md).
