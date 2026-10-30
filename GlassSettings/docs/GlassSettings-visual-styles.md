# GlassSettings — визуальные стили (ThemeManager)

Альтернативный менеджер **системных тем Windows** (`.theme` → `.msstyles`). Не путать с разделом **DWM → Theme** (Atlas / Reflection в реестре OpenGlass).

См. также: [GlassSettings-development.md](GlassSettings-development.md), [GlassSettings-localization.md](GlassSettings-localization.md).

---

## Назначение

| Область | Путь / API |
|---------|------------|
| Страница UI | `GlassSettings/Pages/VisualStylesPage.xaml` |
| Страница «Цвета» | `GlassSettings/Pages/ThemesColorsPage.xaml` |
| Страница «Фон рабочего стола» | `GlassSettings/Pages/ThemesDesktopBackgroundPage.xaml` — hero + спойлер «Персонализируйте фон» |
| Фон (сервис) | `DesktopBackgroundService.cs` — BackgroundType, fit, Solid/Spotlight apply, picture + history |
| Hero-превью | `GlassSettings/Controls/ThemeHeroPreview.xaml` |
| Квадратик chrome (96×96) | `GlassSettings/Controls/ThemePreviewChromeGlyph.xaml` |
| Плитка сетки | `GlassSettings/Controls/VisualStyleTile.xaml` |
| Каталог `.theme` | `GlassSettings/Services/VisualStyleCatalog.cs` |
| User Themes | `UserThemeCatalog.cs` — `Custom.theme` + именованные; сетка: `GetThemesForGrid()` |
| Удаление тем | `ThemeDeleteService.cs`, `ThemeDeleteDialogService.cs` — disabled только стоковые aero/aerolite в `Resources\Themes`; кастом там и `Custom.theme` удаляются; уникальный custom `.msstyles` — папка pack; иначе только `.theme`; активная библиотека — диалог «переключите тему» |
| Save / OG-секция | `UserThemeSaveService`, `OpenGlassThemeSection` (`[Glass.OpenGlass]`), `ThemeFileSectionWriter` |
| Персонализация | `WindowsPersonalizationService`, `AccentColorNameHelper` |
| AppMode / accent (превью) | `ThemeAppModeHelper.cs`, `ThemeAccentColorHelper.cs`, `ThemePreviewChromeModeResolver.cs`, `ThemeDisplayNameResolver.cs` |
| Палитра chrome | `ThemePreviewChromePalette.cs` |
| Обои / watcher | `CurrentWallpaperResolver.cs`, `DesktopEnvironmentWatcher.cs`, `RegistryValueChangeWatcher.cs` |
| Тема окна GS | `WindowThemeBootstrap.cs` — **только** `AppsUseLightTheme` (без `AppMode` из `.theme`) |
| Реестр BWM | `GlassSettingsRegistry.cs` → `EnableSystemFont` |
| Применение темы | `VisualStyleThemeManager.cs`, `ThemeManager2.cs`, `ThemeApplyFallback.cs` |
| Native #65 + DWM | `GlassSettings_ApplyVisualStyleAndDwmPolicy` → `ThemeHelper::SetSystemVisualStyle` + `OpenGlassDwmPolicy` |
| SecureUxTheme | `SecureUxThemeService.cs`, `SecureUxThemeNative.cpp` — папка `SecureUxTheme\`, IFEO `winlogon.exe`, COM ThemeUiProxy |

Навигация: пункт **«Визуальные стили»** (`NavThemes`, tag `themes`). Заголовок — только в breadcrumb. Drill: **Цвета** (`Breadcrumb_ThemesColors`), **Фон рабочего стола** (`Breadcrumb_ThemesDesktopBackground`).

Hero quick-actions (2×2): **Фон** (live-подпись по `BackgroundType`) · **Цвет акцента** · **Цифровая подпись** (тумблер SecureUxTheme4) · **Текст и шрифты**. В шапке спойлера **«Текущая тема»** — имя темы и чекбокс **«Применять шрифты»** (`EnableSystemFont`).

### Подпись «Фон» и страница фона

Источник режима: `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Wallpapers\BackgroundType`.
Подпись кнопки **«Фон»** на хабе: слайд-шоу (`BackgroundType=2` + live) → `Themes_BackgroundSlideshowFormat`; **иначе всегда** `Themes_BackgroundSpotlight` («Windows: интересное»).

Страница **Фон рабочего стола**: `ThemeHeroPreview` + `SettingsSystemGateExpander` с combo режима (Фото / Сплошной цвет / Слайд-шоу / Windows: интересное). **Вход / `RefreshAll` / `WallpaperChanged` UI-refresh** — только sync из реестра/COM (без heal/remember/apply). `TryGetSlideshowAlbum` для UI — read-only (без `Remember*`). Запись — по действию пользователя; heal после apply .theme (`TryNormalizeDegenerateSlideshowToPicture`: ThemeA–D или иной альбом **только при ≤1 картинке** → Фото; многокадровый ThemeC «Восход» не трогать). **Фото** — recent-плитки 100×100 (spacing 3, скругление через ImageBrush); клик по не-первой → сразу move-to-front (`RepositionThemeTransition`), apply `TryApplyPicture` параллельно на STA (без ProgressRing); hero — после apply / кэш; обзор — hosted WinUI picker (`ExecutableFilePickerDialogService.PickImageAsync`, фильтры jpg/png/…); fit (`WallpaperStyle`/`TileWallpaper`, live через watcher). **Слайд-шоу** — combo источника («Фоны Windows» / подпапки Web\Wallpaper) + кнопка «Обзор» → hosted Folder-Picker (`ExecutableFilePickerDialogService.PickFolderAsync`, старт = текущий альбом или `%SystemRoot%\Web\Wallpaper`); строка альбома → hosted-диалог checkbox-плиток (`SlideshowAlbumPickerDialogService`, include = `GetSlideshow` FILESYS, не `ItemNPath`); интервал, shuffle, батарея, fit. **Сплошной цвет** / **Spotlight** — тело скрыто, спойлер collapsed (`AllowBodyExpand=False`), combo активен; Solid apply без палитры (`GetBackgroundColor` / `Colors\Background` → `SetBackgroundColor` + `BackgroundType=1`); Spotlight — `EnabledState=1` + `BackgroundType=3` + asset при наличии.

Подпись «Слайд-шоу (изображений: n)»: число из папки `IDesktopWallpaper.GetSlideshow`, иначе `ItemNPath` / `ImagesRootPath` — **не** из `BackgroundHistoryPath` (там только до 5 недавних). Identity «Фоны…» — `ImagesRootPIDL` / search-folder; `GlassLastSlideshowKind=WindowsDesktopBackgrounds`.

---

## Источники данных

`UserThemeCatalog.GetThemesForGrid()`:

1. именованные `*.theme` из `%LOCALAPPDATA%\Microsoft\Windows\Themes\` (**без** `Custom.theme`), `LastWriteTime` desc;
2. системные из `%SystemRoot%\Resources\Themes\` (top-level).

`Custom.theme` в сетку не попадает — только ссылка «Использовать персонализированную тему».

Для каждого файла:

- `[VisualStyles] Path=` → `.msstyles`;
- `[VisualStyles] ColorStyle=` / `Size=` (default `NormalColor` / `NormalSize`);
- `[Theme] DisplayName=` → имя (`ThemeIndirectStringResolver` для `@…dll,-id`);
- UTF-16 LE (`ThemeFileLineReader`);
- `[VisualStyles] AppMode` / `SystemMode` → `IsDarkMode` (превью плиток; **не** chrome GS);
- `[Control Panel\Desktop] Wallpaper=` и `[Slideshow]` → превью;
- accent из `[Control Panel\Desktop] ColorizationColor`;
- опционально `[Glass.OpenGlass]` — параметры DWM для OpenGlass.

Активная тема: `GetCurrentThemeName` (uxtheme) с fallback на `HKCU\...\Themes\CurrentTheme`.

---

## Применение темы (клик по плитке / Custom.theme)

1. **`IThemeManager2.AddAndSelectTheme`** — pack: обои, звуки, иконки, курсоры, accent; `ThemePackFlagSilent`; `IgnoreScreensaver`.
2. Если чекбокс **«Применять шрифты»** (`EnableSystemFont=1`): **`SetSystemVisualStyle`** (uxtheme ordinal **#65**, **flag = 65**).
3. **DWM policy** (`OpenGlassDwmPolicy::ApplyDwmPolicyOnVisualStyleChange`) — только при `ShouldApplyDwmCompositionPolicy` и если GlassCompat host **не** Running.
4. Если OG exposed и в `.theme` есть `[Glass.OpenGlass]` — запись ключей DWM + `NotifyDwmColorizationNow`.

**Fallback** (если COM недоступен): `#65` + `SPI_SETDESKWALLPAPER` + узкая запись звуков/иконок.

Оркестратор: `VisualStyleThemeManager.ApplyThemeAsync` (STA). После успеха: при живом host — пауза `HostPolicySettleDelayMs` → `ThemeDwmBaselineSync.SettleAndReloadBaselineAsync` (DWM defaults + снимок) → unlock сетки → success-зеркало → затем wallpaper-heal (`TryNormalizeDegenerateSlideshowToPicture`). Notice — слот **Action** (`SettingsNoticeCoordinator`).

---

## Сохранение темы (кнопка «Сохранить»)

Показывается **вместо** «Использовать персонализированную тему», если:
1. OG exposed и live DWM (GlassType / Override / GlassOpacity) **отличается** от fingerprint `v2|…` после apply темы (`LastAppliedThemeDwmFingerprint`); или
2. активна `Custom.theme` (как в Параметрах Windows после смены акцента / персонализации) — даже без OG dirty.

`ColorizationAfterglowOverride` учитывается только как presence (значение меняет тема вместе с акцентом). При первом открытии / миграции формата baseline сидируется с live → без правок DWM и без Custom кнопка скрыта. Во время apply ThemeChanged не обновляет Save (иначе ложный show). После Save baseline обновляется; CurrentTheme обычно именованный `.theme` → Save по п.2 гаснет.

Поток:

1. `IThemeManager2.UpdateCustomTheme`;
2. дописать `[Glass.OpenGlass]` в `Custom.theme`;
3. `CloneAndSetCurrentTheme` (имя из диалога) → именованный `.theme`;
4. если секции нет в копии — дописать;
5. обновить сетку.

Без OG кнопка Save по DWM не показывается.

---

## Страница «Цвета»

ThemesColorsPage — live apply (без page-pipeline Save):

| Спойлер | Тип | Поведение |
|---------|-----|-----------|
| Выберите режим | SettingsSystemGateExpander + AllowBodyExpand | Всегда визуально доступен; разворот только при «Настраиваемый»; тело — SettingsParameterPanel |
| Эффекты прозрачности | SettingsFlatGateRow | Без разворота; gate EnableTransparency |
| Цвет акцента | пикер-спойлер | SettingsColorPickerPanel + Default из исходного `.theme`; в FooterContent: до 10 плиток `ColorHistory0..9` + два тумблера ColorPrevalence; тело развёрнуто и в Авто, и во Вручную (пикер — только Manual) |

**Акцент:** `UseLivePreview=True` — во время drag только UI пикера (без `ColorLivePreview` → система); commit при отпускании / «По умолчанию» через `WindowsPersonalizationService.ApplyAccentColorAsync` (STA: реестр + ImmersiveColorSet + UpdateCustomTheme). На время apply пикер/Default/combo disabled; тумблеры prevalence остаются доступны.

**Недавние цвета:** `HKCU\...\Themes\History\Colors\ColorHistory0..4` (**COLORREF**) — до 5 плиток ~48×48 (как Параметры); select / пикер / Default-baseline — `AccentPalette[3]` (`ReadUserAccentColor`), не `DWM\AccentColor` и не `ColorizationColor` из `.theme` (часто другой оттенок chrome); клик → `ApplyAccentColorAsync`. После apply темы baseline Default обновляется из live user accent; `PushRecentAccentColor` сдвигает историю.

**ColorPrevalence (как Параметры Windows):**
| Тумблер | Ключ |
|---------|------|
| Пуск и панель задач | `HKCU\...\Themes\Personalize\ColorPrevalence` |
| Заголовки и границы окон | `HKCU\...\Windows\DWM\ColorPrevalence` |

Пуск/панель: `IsEnabled=false` при `SystemUsesLightTheme=1`. Apply цвета **не** пишет `DWM\ColorPrevalence`.

**Внешний sync (DesktopEnvironmentWatcher → ReloadFromSystem):**
- Personalize RegNotify: `AppsUseLightTheme` + `SystemUsesLightTheme` + `EnableTransparency` + `ColorPrevalence` → fingerprint в ThemeChanged;
- Desktop RegNotify: `AutoColorization` + DWM AccentColor/Colorization*/ColorPrevalence → AccentChanged.

**Default акцента:** ColorizationColor из исходного `.theme` — `LastAppliedThemeFilePath` (после apply на Visual Styles) или CurrentTheme, если это не `Custom.theme`. Без fallback на текущий реестровый accent; нет исходника → кнопка disabled.

Карточка «Цвет акцента» на Visual Styles: subtitle = ближайшее имя акцента (AccentColorNameHelper), с заглавной.

Форматы (один RGB): stock `.theme` и DWM `ColorizationColor`/`Afterglow` — **0xAARRGGBB**; DWM/Explorer `AccentColor*` — **COLORREF 0xAABBGGRR**. После `UpdateCustomTheme` ключи снова пишутся в этих форматах.


---

## Hero-превью

- Обои: `CurrentWallpaperResolver`; обновление через RegNotify Desktop + `DesktopEnvironmentWatcher`.
- **Crossfade обоев:** [GlassSettings-wallpaper-preview-crossfade.md](GlassSettings-wallpaper-preview-crossfade.md).
- Масштаб / chrome overlay — без изменений (см. историю гайда).

---

## Синхронизация Light/Dark окна GlassSettings

`WindowThemeBootstrap.ResolveElementTheme()` читает **только** `HKCU\...\Themes\Personalize\AppsUseLightTheme` (паритет обычных приложений при `AppTheme = Системный`).

`MainWindow` + `DesktopEnvironmentWatcher`: `ThemeChanged` / `AccentChanged` → `SyncWindowThemeFromDesktop` при `ShouldFollowDesktopTheme()`.

**Порядок с GlassCompat host (анти-гонка):** host пишет DWM policy на `WM_THEMECHANGED`; GS — reader + `ThemeDwmBaselineSync`.

---

## UI (Visual Styles)

1. **Hero** + быстрые действия: системные шрифты, **Цвет**, сглаживание текста, toggle «Шрифты темы»; **Сохранить** *или* «Использовать персонализированную тему» (`Custom.theme`).
2. **Текущая тема** (Expander) + сетка (именованные user → системные). ПКМ → «Удалить»: disabled только у стоковых aero/aerolite (`IsDefaultVisualStylePath` + файл в `Resources\Themes`). Кастомные `.theme` в том же каталоге и `Custom.theme` — пункт enabled. Если `.msstyles` больше не ссылается другими `.theme` и это не aero/Program Files — удаляется `.theme` и папка библиотеки (ресурсы); иначе только `.theme`. FullLibrary активной темы / того же `.msstyles` — hosted OK «сначала переключите тему», без удаления. `ThemeDeleteService.TryCreatePlan` / `Execute`.
3. Подвал: «Обзор тем» → MenuFlyout (`7themes.su`, `vsthemes.org`).
4. **Цифровая подпись** — тумблер IFEO/Application Verifier. Payload: папка `SecureUxTheme\` рядом с `GlassSettings.exe` (`SecureUxTheme4_64.dll` / `_32.dll`, `ThemeUiProxy_64.dll` / `_32.dll`; при копировании постфикс снимается). GS x64: патчер и proxy в System32, `ThemeUiProxy_32.dll` в SysWOW64; `SecureUxTheme4_32.dll` на x64 не копируется. При ON: COM `InProcServer32` → `ThemeUiProxy.dll`, IFEO `winlogon.exe`. При OFF: COM → `themeui.dll`, IFEO снимается; DLL удаляет одноразовая задача ONSTART (SYSTEM) скриптом из `%TEMP%`. Официальный MSI — блокер, пока ключ продукта в реестре; после удаления MSI leftover `SecureUxTheme4.dll` / `ThemeUiProxy.dll` в System32 **не** блокер: копирование перезаписывает их, а если файл занят winlogon или WRP (`ERROR_SHARING_VIOLATION` / `ERROR_ACCESS_DENIED`) — существующий dest оставляем и продолжаем IFEO+COM (перезагрузка не обязательна). Старый ThemeTool (`SecureUxTheme.dll`) — по-прежнему блокер. Внизу страницы (под спойлером тем) — ссылка «SecureUxTheme в интернете» (`https://github.com/namazso/SecureUxTheme`).

---

## Реестр GlassSettings

`HKCU\Software\StanislavKa\GlassPlatform\GlassSettings`:

| Значение | Тип | Назначение |
|----------|-----|------------|
| `EnableSystemFont` | DWORD 0/1 | Toggle «Шрифты темы»; при apply → flag **65** для `#65` |

API: `GlassSettingsRegistry.ReadEnableSystemFont` / `WriteEnableSystemFont`.

---

## Секция `[Glass.OpenGlass]`

Ключи (DWORD hex), по режиму Vista/Win7: `GlassType`, `ColorizationColor`, `ColorizationColorInactive` / `GlassOpacity` / `GlassOpacityInactive`, `ColorizationAfterglow` + balances, `ColorizationBlurBalance`.

---

## Roadmap (осталось)

1. Импорт произвольного `.theme` / `.deskthemepack`.

---

## Строки локализации

Префикс `Themes_*`, `Themes_Colors_*`, `Themes_ColorName_*`, `Breadcrumb_ThemesColors`.
