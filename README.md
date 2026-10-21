# Glass Platform

Glass Platform — глубокая кастомизация внешнего вида Windows 10 и 11: прозрачность, «стекло», согласованные цвета и мягкий свет в окнах, на панели задач, в меню «Пуск», в центре уведомлений и в контекстных меню. Это те же окна и те же элементы системы, но с другой отрисовкой.

Платформа **не** рисует второй слой «поверх» рабочего стола. Хост подключается к графическим процессам, перехватывает штатную отрисовку (GDI и связанные пути) и подменяет то, **как** рисуются рамки, фоны, кнопки, полосы прокрутки, всплывающие окна и текст: кисти, цвета, порядок закраски, отдельные эффекты материала. Логика окон, меню и ярлыков не заменяется — меняется только визуальный результат.

Облик частично вдохновлён [TranslucentFlyouts](https://github.com/ALTaleX531/TranslucentFlyouts). Установщик по желанию может положить [OpenGlass](https://github.com/ALTaleX531/OpenGlass) в каталог Glass. **GlassPlatform является продолжением легендарного, но устаревшего BlackWindowManager, однако GlassPlatform не совместима с BlackWindowManager** и не использует его настройки как источник истины.

Glass Platform is experimental desktop theming for Windows 10/11: it hooks into GUI processes and replaces how controls and shell surfaces are painted. License: GNU GPL v3.

## Что вы получаете

- **Окна и диалоги** — нестандартные рамки и фоны, согласованные с цветом рабочего стола.
- **Стандартные элементы** — кнопки, списки, комбобоксы, панели инструментов и прочие common controls в едином стеклянном стиле.
- **Панель задач** — сегментированная отрисовка полосы и её элементов.
- **Меню «Пуск», поиск, flyout-окна** — через модули оболочки (в том числе XAML-острова проводника, где это применимо).
- **Центр уведомлений и календарь** — оформление в общей стеклянной логике.
- **Контекстные меню** — стеклянные меню только Win32 и некоторые UWP на панели задач.
- **Расширение в контекстное меню** — пункты управления платформой с рабочего стола.
- **Проводник** — окна папок и связанные поверхности в одной палитре с остальной оболочкой.

Параметры хранятся в **GlassSettings** (WinUI). Включение и выключение — **LogViewer** или контекстное меню; платформа не требует править реестр вручную для повседневной работы. Справочник ключей — в разделе [Реестр](#реестр).

## Прежде чем включать

Программа **экспериментальная**: инъекция в GUI-процессы, дополнительная нагрузка на CPU и GPU, без гарантии стабильности. После остановки возможны кратковременные артефакты в проводнике или других процессах, пока они не перерисуются. Не используйте на машине, где сбой или перезапуск оболочки недопустимы.

Подробнее о рисках и отказе от гарантии — [LICENSE](LICENSE).

## Требования

- Windows 10 или 11 (основная разработка и запуск — x64; x86 собирается отдельно)
- права администратора для старта хоста платформы
- Visual Studio 2022 — только для сборки из исходников

## Сборка из исходников

Заголовки библиотеки [gtl](https://github.com/greg7mdp/gtl) положите в каталог `gtl` в корне решения, рядом с `GlassProject.sln`. Сборка подключает этот каталог и читает из него `phmap.hpp` (`gtl\phmap.hpp`). В репозиторий каталог не входит. Каталог `tools` тоже локальный и на GitHub не публикуется.

```bat
msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64
```

Результат: `Build\x64\Debug\`.

**Запуск:** LogViewer → START, или `Rundll32.exe "путь\GlassCompat.dll",Main /start`.

**Остановка:** STOP в LogViewer, `Main /stop`, или `stop.vbs` в каталоге сборки, если платформа уже запущена. Если MSBuild пишет, что DLL занята — сначала остановите платформу штатно, не убивайте процессы без нужды.

**Настройки:** GlassSettings.exe. Регистрация пункта контекстного меню рабочего стола — из GlassSettings.

## Состав

| Модуль | Роль |
|--------|------|
| GlassCompat.dll | Хост: WinEvent, инжекция, остановка |
| GlassCore.dll | Перехват отрисовки, темы, диспетчер paint |
| WndCompat.dll | Подклассы и окна приложений |
| ExplorerCompat.dll | Проводник |
| TaskbarCompat.dll | Панель задач |
| UICompat.dll | CoreWindow / «Пуск», поиск, NC |
| GlassSettings.exe | Параметры |
| LogViewer.exe | Управление и журнал (только в Debug) |
| GlassShellExt.dll | Контекстное меню рабочего стола |
| GlassSetup.exe | Установщик платформы |

## Реестр

Настройки платформы лежат в **`HKCU\Software\StanislavKa\GlassPlatform`**. Тот же путь в `HKLM` используется только как запасное чтение для `InstallPath` и бинарников свечения текста. Прежний ключ `Software\StanislavKa\BlackWindowManager` больше не читается и не пишется; значения из него сами не переносятся.

Нет значения — действует умолчание. GlassSettings в этом случае обычно удаляет параметр, а не записывает заводское число. Цвета ниже — `DWORD` в формате **AARRGGBB**.

### Параметры DWM (OpenGlass)

Параметры DWM здесь не перечисляются. У OpenGlass их больше, чем у страниц Glass, и описание каждого значения — в оригинале: [Configuration and registry reference](https://github.com/ALTaleX531/OpenGlass/wiki/Configuration-and-registry-reference).

GlassSettings пишет часть этих значений в `HKCU\Software\Microsoft\Windows\DWM` со страниц цвета, стекла и темы. Полный набор, типы и смысл смотрите по ссылке выше, а не в таблицах этого файла.

### Popup: общие настройки и подразделы

Два режима. Раздельный включается, когда параметры оформления уже есть хотя бы в одном из разделов `Menu`, `DropDown`, `ToolTip`.

**Общие настройки.** Эффект, градиент, обводка, углы и анимация лежат в корне `HKCU\Software\StanislavKa\GlassPlatform`. Анимация — в `Animation`. Один набор действует на меню, выпадающие списки и подсказки. Разделы `Menu`, `DropDown` и `ToolTip` в этом режиме не заполняются.

**Раздельные настройки.** Те же группы пишутся в `Menu`, `DropDown` и `ToolTip`. Анимация — в `{раздел}\Animation`.

Группа в разделе перекрывает одноимённую группу в корне. Если в разделе группы нет, а в корне она есть, берётся корень. В интерфейсе это пометка «Задан общий параметр».

| Группа | Свой набор в разделе, если есть хотя бы одно из значений |
|--------|----------------------------------------------------------|
| Эффект | `EffectType` |
| Градиент | `DarkMode_GradientColor`, `LightMode_GradientColor` |
| Обводка | `DarkMode_BorderColor`, `LightMode_BorderColor`, `NoBorderColor` |
| Углы | `CornerType`, `EnableDropShadow` |
| Анимация | любой параметр в `{раздел}\Animation` |

Пункты меню, тень и фон иконок меню, цвет и отступы подсказки есть только в раздельном режиме: `Menu`, `Menu\Hot`, `Menu\DisabledHot`, `Menu\Focusing`, `Menu\Separator`, `ToolTip`.

### Root: один набор и состояние окна

`SeparateActiveInactive` в `Root`: `0` — без учёта состояния, `1` — по состоянию окна. Нет ключа — по состоянию, если в `Root\Inactive` уже есть параметры оформления.

**Без учёта состояния.** Эффект, градиент, обводка и `GlassDrawThemeConstantAlpha` лежат в `Root` и применяются и к активному, и к неактивному окну.

**По состоянию окна.** Эффект, градиент и обводка — в `Root\Active` и `Root\Inactive`. Нет своего значения в состоянии — берётся одноимённый параметр из `Root`. `GlassDrawThemeConstantAlpha` в этом режиме пишется в `Root\Active` и `Root\Inactive`.

`NoBackdrop` и `CornerType` всегда в `Root`. В `Root\Active` и `Root\Inactive` углы не пишутся.

Тот же набор эффекта, градиента, обводки, углов и анимации, что в таблицах ниже, ещё используется панелью задач: `Tray`, `Tray\Xaml` и `Tray\Xaml\Popup`, `Preview`, `Menu`, `Tooltip`. У соседнего окна XAML нет своего цвета — берётся значение из `Tray\Xaml`. Анимация раздела — в `{раздел}\Animation`.

### Оформление

| Параметр | Описание |
|----------|----------|
| `EffectType` | Вид фона. `0` отключить, `1` прозрачный (SWCA), `2` сплошная, `3` размытие, `4` классический акрил, `5` современный акрил, `6` акрил (DWM), `7` Mica, `8` Mica Alt. Отсутствие ключа — это не «отключить» |
| `DarkMode_GradientColor` | Цвет фона в тёмной теме. Не применяется при `EffectType` 6–8 |
| `LightMode_GradientColor` | То же для светлой темы |
| `DarkMode_BorderColor` | Цвет обводки в тёмной теме. На Windows 11 при скруглённых углах альфа может игнорироваться |
| `LightMode_BorderColor` | То же для светлой темы |
| `NoBorderColor` | `1` — не задавать свой цвет обводки |
| `CornerType` | Форма углов: `0` системные, `1` прямые, `2` крупное скругление, `3` мелкое. Для XAML-панели задач шкала длиннее, см. примечание 1. На `Root` пишется в сам раздел `Root` и только на Windows 11 |
| `EnableDropShadow` | `1` — тень |

### Анимация (`Animation`)

| Параметр | Описание |
|----------|----------|
| `EnableFluentAnimation` | `1` — fluent-анимация раздела. В ползунках страницы не показывается |
| `PopInStyle` | Стиль появления: `0` сдвиг вниз, `1` волна, `2` сдвиг вправо, `3` масштабирование. По умолчанию `0` |
| `FadeOutTime` | Затухание, мс. По умолчанию 350 |
| `PopInTime` | Появление, мс. По умолчанию 250 |
| `FadeInTime` | Проявление, мс. По умолчанию 87 |
| `StartRatio` | Начальный масштаб или сдвиг, 0–100 %. По умолчанию 50 |
| `ImmediateInterrupting` | `1` — сразу прервать предыдущую анимацию. По умолчанию `0` |

### Меню (`Menu`)

| Параметр | Описание |
|----------|----------|
| `EnableCustomRendering` | `1` — своя отрисовка пунктов. Выключение стирает параметры видов пункта |
| `NoSystemDropShadow` | `1` — без системной тени. По умолчанию `0` |
| `EnableImmersiveStyle` | `1` — современный стиль меню. По умолчанию `1` |
| `NoModernAppBackgroundColor` | `1` — без фона UWP-иконок. По умолчанию `1` |
| `ColorTreatAsTransparent` | Цвет фона иконки, который вырезается от углов к центру. Наличие значения включает удаление |
| `ColorTreatAsTransparentThreshold` | Насколько близкий цвет ещё считается тем же. По умолчанию 50 |

Виды пункта — `Menu\Hot` (наведение), `Menu\DisabledHot` (наведение на неактивный пункт), `Menu\Focusing` (рамка клавиатурного фокуса), `Menu\Separator` (разделитель). Имена значений общие.

| Параметр | Описание |
|----------|----------|
| `Disabled` | `1` — не рисовать этот вид. По умолчанию `0` |
| `CornerRadius` | Скругление подложки или рамки. По умолчанию 8 |
| `EnableThemeColorization` | `1` — цвет из акцента Windows вместо пикера |
| `DarkMode_Color` | Цвет в тёмной теме |
| `LightMode_Color` | Цвет в светлой теме |
| `Width` | Толщина штриха у `Focusing` и `Separator`. 1000 ≈ 1 px. По умолчанию 1000 |
| `DarkMode_ThemeColorizationType` | Строка типа цвета темы. По умолчанию `ImmersiveStartHoverBackground`; такое значение в реестр не пишется |
| `LightMode_ThemeColorizationType` | То же для светлой темы |

### Подсказки (`ToolTip`)

Помимо общего оформления:

| Параметр | Описание |
|----------|----------|
| `DarkMode_Color` | Цвет подсказки в тёмной теме. Заводской `0xFFFFFFFF`, пока ключ не записан |
| `LightMode_Color` | Цвет в светлой теме. Заводской `0xFF1A1A1A` |
| `MarginsType` | `0` добавить отступы, `1` заменить |
| `Margins_cxLeftWidth` | Левый отступ. По умолчанию 6 |
| `Margins_cxRightWidth` | Правый отступ. По умолчанию 6 |
| `Margins_cyTopHeight` | Верхний отступ. По умолчанию 6 |
| `Margins_cyBottomHeight` | Нижний отступ. По умолчанию 6 |

### Окна (`Root`)

| Параметр | Описание |
|----------|----------|
| `NoBackdrop` | `1` — не применять backdrop (переключатель «Сплошное стекло» выключен). Нет ключа — backdrop разрешён |
| `SeparateActiveInactive` | `0` — один набор для активного и неактивного окна, `1` — раздельно (`Root\Active` и `Root\Inactive`). Нет ключа — раздельный режим, если в `Root\Inactive` уже есть параметры |
| `CornerType` | Углы окна на Windows 11. Шкала как в таблице оформления |
| `GlassDrawThemeConstantAlpha` | В `Root\Active` и `Root\Inactive`: непрозрачность DrawTheme, 0–255. Нет ключа — 255 |

Удаление фона иконок — `Root\Control\Ribbon`, `ListView`, `TreeView`, `Toolbar`, `Button`, `Static`. В каждом разделе два значения, как у иконок меню: `ColorTreatAsTransparent` и `ColorTreatAsTransparentThreshold` (по умолчанию цвет `0xFFFFFFFF`, порог 50).

### Панель задач (`Tray`)

Общее оформление панели — в `Tray`. Путь XAML — в `Tray\Xaml` и в `Tray\Xaml\Popup`, `Preview`, `Menu`, `Tooltip`.

| Параметр | Где | Описание |
|----------|-----|----------|
| `BorderDrawMode` | `Tray` | `0` — бордюр DWM, ненулевое — обводка XAML |
| `SeparateZones` | `Tray\Xaml` | `1` — панель из отдельных групп |
| `CornerTypeSchema` | `Tray\Xaml` | Служебная метка шкалы углов. Текущая схема — `3`. См. примечание 1 |
| `CachedSystemFillArgb` | `Tray\Xaml` | Снимок системной заливки. Не настройка |
| `CachedSystemStrokeArgb` | `Tray\Xaml` | Снимок системной обводки для «По умолчанию». Не настройка |

### Текст

В корне ключа, отдельно для тёмной и светлой темы. Нет значения или значение равно заводскому — ключ удаляется.

| Параметр | Описание |
|----------|----------|
| `DarkMode_GlobalTextGlow` | Свечение текста, `REG_BINARY` 16 байт. См. примечание 2 |
| `LightMode_GlobalTextGlow` | То же для светлой темы |
| `GlassInkAlphaCapsule` | Профиль чернил текста, `REG_BINARY` 100 байт. См. примечание 3 |

Системные шрифты страница «Текст» читает и пишет как `REG_BINARY` в `HKCU\Control Panel\Desktop\WindowMetrics`: `CaptionFont`, `SmCaptionFont`, `MenuFont`, `StatusFont`, `MessageFont`, `IconFont`.

### Процессы и служебные разделы

| Раздел | Описание |
|--------|----------|
| `AttachList` | Имя значения — имя exe. `DWORD`: `0` не внедрять, `1` только всплывающие окна, `2` полностью |
| `BlockList` | Имя значения — имя exe. `DWORD` `1` — процесс в пользовательском списке блокировки. Встроенный список в реестр не пишется |
| `DisabledList` | GlassSettings не редактирует и не включает в снимок настроек |
| `Crashes\DumpFolder` | Строка: каталог минидампов. Если пусто, то же значение ищется в корне ключа, затем в `HKCU\Software\Glass\CrashGuard`. Иначе `%LOCALAPPDATA%\GlassPlatform\Crashes` |
| `GlassSettings\MainWindowPlacement` | Положение окна GlassSettings, не параметр оформления |

### Окно GlassSettings (`GlassSettings`)

| Параметр | Описание |
|----------|----------|
| `EnableSystemFont` | `1` — системный шрифт интерфейса |
| `AppTheme` | Тема окна: `0` как в системе, `1` светлая, `2` тёмная. `0` не хранится |
| `WindowBackdrop` | Фон окна: `0` системный, `1` акрил, `2` Mica, `3` Mica Alt. `0` не хранится |
| `WindowBusyOverlayMode` | Штора ожидания: `0` акрил, `1` затемнение. `0` не хранится |
| `AppUiLanguage` | Строка BCP-47 (`ru-RU`, `en-US`). Нет значения — язык системы |
| `UpdateCheckInterval` | Проверка обновлений: `0` каждый день, `1` неделю, `2` месяц, `3` никогда |
| `ExecutableFilePickerLastPath` | Последняя папка выбора exe |
| `LastAppliedThemeFilePath` | Последний применённый файл `.theme` |
| `LastAppliedThemeAccentColor` | Акцент этой темы, база для «По умолчанию» на странице цветов |
| `LastAppliedThemeDwmFingerprint` | Отпечаток DWM после применения темы; от него считается кнопка «Сохранить» |
| `OpenGlassUpdateLastCheckUtc` | Время последней проверки обновления OpenGlass |
| `OpenGlassUpdatePendingVersion` | Версия, которую можно поставить |
| `OpenGlassUpdatePendingUrl` | Адрес этой версии |
| `OpenGlassUpdateNoticeDismissedVersion` | Версия, для которой баннер уже закрыт |

### Каталог установки

| Ключ | Описание |
|------|----------|
| `HKCU\Software\GlassPlatform\InstallPath` и `HKCU\Software\Wow6432Node\GlassPlatform\InstallPath` | Каталог установки, который записывает установщик. Для x86 из 64-разрядного процесса читается ветка Wow6432Node |
| `HKCU` и `HKLM` `\Software\StanislavKa\GlassPlatform\InstallPath` | Запасной путь, по которому GlassSettings ищет OpenGlass, если каталог установки не задан первым ключом |

### Ключи Windows

GlassSettings меняет их вместе с обоями, акцентом и темой. Это ключи системы, не ключ платформы. Улей — `HKCU`.

| Ключ | Параметры |
|------|-----------|
| `Software\Microsoft\Windows\CurrentVersion\Themes\Personalize` | `AppsUseLightTheme`, `SystemUsesLightTheme` (`1` светлая тема), `EnableTransparency`, `ColorPrevalence` (`1` — акцент на заголовках и панели задач) |
| `Software\Microsoft\Windows\CurrentVersion\Explorer\Accent` | `AccentColorMenu`, `StartColorMenu` — акцент меню |
| `Software\Microsoft\Windows\CurrentVersion\Themes\History\Colors` | `ColorHistory0` … недавние акценты |
| `Software\Microsoft\Windows\DWM` | `AccentColor`, `ColorPrevalence` — акцент Windows. Остальные значения DWM — параметры OpenGlass; их больше, чем в таблицах Glass, см. [справочник OpenGlass](https://github.com/ALTaleX531/OpenGlass/wiki/Configuration-and-registry-reference) |
| `Control Panel\Desktop` | `Wallpaper`; `AutoColorization` (`1` — акцент с обоев); `WallpaperStyle` и `TileWallpaper` задают кадрирование, см. примечание 4 |
| `Control Panel\Colors` | `Background` — цвет сплошного фона, строка `R G B` |
| `Control Panel\Personalization\Desktop Slideshow` | `ImagesRootPath`, `Shuffle` (`1` — перемешать), `Interval` (мс: 60000, 600000, 1800000, 3600000, 21600000, 86400000) |
| `Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers` | `BackgroundType`: `0` фото, `1` сплошной цвет, `2` слайд-шоу, `3` Spotlight. `GlassLastSlideshowFolder` — папка альбома. `GlassLastSlideshowKind`: `Folder` или `WindowsDesktopBackgrounds`. `SlideshowSourceDirectoriesSet` — служебный флаг |
| `Software\Microsoft\Windows\CurrentVersion\DesktopSpotlight\Settings` | `EnabledState` — Spotlight включён |

### Примечания

1. **Углы XAML-панели.** `CornerType` в `Tray\Xaml` и соседних разделах: `0` системные, `1` прямые, `2` крупное, `3` мелкое, `4` увеличенное, `5` максимальное. `CornerTypeSchema` = `3` означает эту шкалу. Если метка схемы меньше 2, старое значение `4` (тогда это было максимальное скругление) переписывается в `5`.

2. **`GlobalTextGlow`.** Четыре `DWORD` подряд: включено (`0`/`1`), радиус в тысячных долях (0–1000, заводской 300), цвет AARRGGBB, режим распространения (`0` только popup, `1` умеренно на окнах, `2` глобально).

3. **`GlassInkAlphaCapsule`.** `DWORD` сигнатуры `GVT1` (`0x31545647`), режим (`0` авто, `1` один профиль, `2` два), зарезервированный `DWORD`, затем два профиля по 44 байта. Поля профиля, по порядку: гамма покрытия в тысячных, `PreserveClearTypeTint`, масштаб альфы в тысячных, усиление бахромы в тысячных, добавка альфы, целевая альфа смешения, уровень серого фона, смягчение бахромы в тысячных, псевдосвечение (`0`/`1`), его радиус в тысячных, его интенсивность. Часть старых полей интерфейс записывает нейтральными: сохранение ClearType `0`, оба масштаба `1000`, добавка альфы `0`, серый фон `0`.

4. **Кадрирование обоев.** Пара (`WallpaperStyle`, `TileWallpaper`): заполнить `10`/`0`, вписать `6`/`0`, растянуть `2`/`0`, замостить `0`/`1`, по центру `0`/`0`, на несколько мониторов `22`/`0`.
