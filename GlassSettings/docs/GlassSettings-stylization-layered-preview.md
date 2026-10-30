# GlassSettings — каскадное превью «Стилизация»

Главное превью в левом верхнем углу страницы **«Стилизация»** (`DwmThemeDetailPage`,
breadcrumb `Dwm_HubTheme` — **не** «Визуальные стили»/`VisualStylesPage`). Показывает
текущую конфигурацию темы как стопку из 5 каскадных слоёв с 3D-тенью, названием и
тултипом-именем файла на каждом слое.

**Crossfade обоев и стекла:** [GlassSettings-wallpaper-preview-crossfade.md](GlassSettings-wallpaper-preview-crossfade.md).

## Слои (снизу вверх)

| # | Слой | Источник (custom) | Источник по умолчанию |
|---|------|-------------------|------------------------|
| 1 | Обои | — | текущие обои рабочего стола (`CurrentWallpaperResolver` + shared pipeline `DisplayWallpaper`) |
| 2 | Стекло | — | **blur + glow (Win7) + tint** из shared pipeline «Композиции» (см. ниже) |
| 3 | Отражение | реестр `CustomThemeReflection` | PNG-ресурс **102** (`"PNG"`) из `OpenGlass.dll` — **первая плитка** коллекции |
| 4 | Материал | реестр `CustomThemeMaterial` | RCDATA-ресурс **2000** (`"#10"`) из системного `Windows.UI.Xaml.Controls.dll` |
| 5 | Окно | реестр `CustomThemeAtlas` (+legacy `AtlasThemeImage`) | атлас **текущего визуального стиля** через theme API |

Каждый слой меньше предыдущего на **30 px** снизу и справа (`Inset`), общий верхний-левый
угол. Слой приподнят по `Translation` Z и отбрасывает `ThemeShadow` на нижние.

### Слой «Стекло»

Не использует `PreviewGlassAcrylicBrush`. Собирается как на hero «Композиции»:

- **Vista** (`GlassType` ≠ 1): gaussian blur + flat tint активного стекла (`ColorizationColor` + `GlassOpacity` через `CompositionPreviewRegistryStates`).
- **Win7 Aero**: blur (`ColorizationBlurBalance`) + glow (afterglow) + tint (`ColorizationColorBalance`).

Pipeline: [`CompositionWallpaperPipelineBuilder`](../../GlassSettings/Services/CompositionWallpaperPipelineBuilder.cs) **316×168**; cache hit — [`CompositionWallpaperLayersCache`](../../GlassSettings/Services/CompositionWallpaperLayersCache.cs).

**Смена обоев:** [`ApplyCombinedPipelineSnapshot`](../../GlassSettings/Controls/StylizationLayeredPreview.xaml.cs) — blur, glow и `DisplayWallpaper` crossfade **синхронно** (500 ms). Отдельный decode обоев из cache **до** pipeline не применяется.

### Слой «Отражение»

Текстура **не масштабируется**: `ImageBrush` + `Stretch=None`, `AlignmentX/Y=Left/Top`, clip = габарит слоя каскада (верхний левый угол, 1:1). Прозрачность — `ColorizationGlassReflectionIntensity` (gate).

## Коллекции отражения и материала

| Аспект | Отражение | Материал |
|--------|-----------|----------|
| Default-плитка | PNG **102** из OG, первая в списке | RCDATA **2000**, первая в списке |
| Выбор default | удаляет `CustomThemeReflection` из реестра; галочка на default | аналогично `CustomThemeMaterial` |
| Gate OFF | удаляет custom + `ColorizationGlassReflectionIntensity`; слой скрыт в превью | удаляет custom + `MaterialOpacity` |
| Layout сетки | ширина по `maxColumns` (до 5), выравнивание с material | то же |

## Где код

| Слой | Файл | Роль |
|------|------|------|
| Контрол | `GlassSettings/Controls/StylizationLayeredPreview.{xaml,xaml.cs}` | каскад 5 слоёв, glass pipeline, wallpaper sync crossfade |
| Страница | `GlassSettings/Pages/Dwm/DwmThemeDetailPage.{xaml,xaml.cs}` | коллекции, gate, `LayeredPreview.RefreshLayers()` |
| C# оркестрация | `GlassSettings/Services/ThemeImageCatalog.cs` | ресурсы, превью атласа |
| Нативные экспорты | `GlassSettings/Native/AtlasPreviewGenerator.cpp` | extract resource, theme API atlas |
| Строки | `Strings/{ru-RU,en-US}/Resources.resw` | `Stylization_Layer_*`, `Dwm_DefaultReflectionTile` |

## Дефолты из «живых» ресурсов

- **Отражение и материал** — `GlassSettings_ExtractResourceImage` из DLL (см. прежний раздел в git history).
- **Окно текущего стиля** — `GlassSettings_GenerateCurrentThemeAtlasPreview` через theme API + `CompositePreview`.

## Поведение и обновление

- `RefreshLayers()` — из `DwmThemeDetailPage.Reload()`; обои+стекло через pipeline при смене identity.
- `RefreshWallpaperLayer()` — `DesktopEnvironmentWatcher.WallpaperChanged` (debounce на странице).
- `RefreshLayerEffects()` — только opacity reflection/material (слайдеры gate).
- Слой «Окно» — async atlas с `_refreshGeneration`.

## Связь с «Композицией»

Архитектура каскада (`Inset`, `ThemeShadow`, подписи) общая с [`DwmCompositionCascadePreview`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml). На «Стилизации» слой стекла — **композит** blur/glow/tint в одном слоте каскада (inset 30), а не отдельные карточки как на «Композиции» (inset 24).

**«Текст и шрифты»:** тот же pipeline и выбор Vista/Win7 по `GlassType`; каскад обои → одна карточка стекла (blur+glow+tint+ink внутри), без отдельных карточек blur/glow/tint (`PreviewKind=WallpaperBlurBase` в [`DwmCompositionCascadePreview`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml)).

См. также: [конструкция миниатюры атласа](GlassSettings-atlas-preview.md).
