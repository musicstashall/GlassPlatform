# GlassSettings — crossfade обоев в превью

Единая механика плавной смены обоев (и связанных bitmap-слоёв) во всех hero-превью и карточках GlassSettings.

См. также: [GlassSettings-stylization-layered-preview.md](GlassSettings-stylization-layered-preview.md), [DwmColorizationPage.md](DwmColorizationPage.md), [GlassSettings-visual-styles.md](GlassSettings-visual-styles.md), [GlassSettings-startup-white-flash.md](GlassSettings-startup-white-flash.md).

---

## Централизованная реализация

### Длительность

| Константа | Значение | Где |
|-----------|----------|-----|
| `WallpaperPreviewCrossfade.DefaultCrossfadeDurationMs` | **500 ms** | [`WallpaperPreviewCrossfade.cs`](../../GlassSettings/Controls/WallpaperPreviewCrossfade.cs) |

Дефолтный аргумент конструктора `WallpaperPreviewCrossfade(..., fadeDurationMs = DefaultCrossfadeDurationMs)` — все новые вызовы без явной длительности получают **500 ms**.

[`UiElementCrossfade`](../../GlassSettings/Controls/UiElementCrossfade.cs) для blur/glow принимает ту же длительность явно (composition/stylization передают `DefaultCrossfadeDurationMs`).

**Исключение (не обои):** crossfade Vista↔Win7 на странице «Композиция» между **двумя** экземплярами `DwmCompositionCascadePreview` — [`UiElementCrossfade`](../../GlassSettings/Controls/UiElementCrossfade.cs) с **`PivotTabContentTransition.TabChangeTotalDuration` (500 ms)**; это смена режима Glass type, не смена обоев.

### Double-buffer и easing

| Компонент | Роль |
|-----------|------|
| [`WallpaperPreviewCrossfade`](../../GlassSettings/Controls/WallpaperPreviewCrossfade.cs) | два `Image` (A/B) + fallback; `CubicEase` InOut; первый кадр — **без** анимации |
| [`UiElementCrossfade`](../../GlassSettings/Controls/UiElementCrossfade.cs) | два `UIElement` (blur/glow rectangles); та же длительность; уважает `UISettings.AnimationsEnabled` |

Правило animate: `_hasWallpaperImage` / наличие предыдущего `ImageSource` на blur-brush — **первый показ** instant, последующие смены identity — crossfade.

### Shared decode и pipeline

| Сервис | Назначение |
|--------|------------|
| [`DesktopWallpaperImageCache`](../../GlassSettings/Services/DesktopWallpaperImageCache.cs) | единый decode **340×192** для простых превью (hero tile, visual styles) |
| [`CompositionWallpaperPipelineBuilder`](../../GlassSettings/Services/CompositionWallpaperPipelineBuilder.cs) | sharp → gaussian → blur/glow BGRA + `DisplayWallpaper` **340×192** |
| [`CompositionWallpaperLayersCache`](../../GlassSettings/Services/CompositionWallpaperLayersCache.cs) | фоновый prewarm Vista+Win7 (316×168 blur) при cold start |
| [`CompositionPreviewRegistryStates`](../../GlassSettings/Services/CompositionPreviewRegistryStates.cs) | `PreviewState` из HKCU без UI gate |

**Identity обоев:** `CurrentWallpaperResolver.TryGetWallpaperImageColorIdentity()` (не только path) — триггер refresh при `ImageColor` / transcoded wallpaper.

### Синхронный snapshot (composition / stylization glass)

На страницах с blur/glow **нельзя** обновлять обои из `DesktopWallpaperImageCache` отдельно от blur — иначе desync анимации.

Паттерн (как [`ApplyPipelineSnapshot`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml.cs) на «Композиции»):

1. Смена wallpaper identity → **не** мгновенный crossfade из cache decode.
2. `RefreshWallpaperPipelineAsync` / `CompositionWallpaperPipelineBuilder.Build` (или cache hit).
3. В **одном** apply: crossfade blur → glow (Win7) → crossfade `payload.DisplayWallpaper`.

Стилизация: [`ApplyCombinedPipelineSnapshot`](../../GlassSettings/Controls/StylizationLayeredPreview.xaml.cs) в [`StylizationLayeredPreview`](../../GlassSettings/Controls/StylizationLayeredPreview.xaml.cs).

---

## Потребители по страницам

| Страница | Контрол | Что анимируется | Особенности |
|----------|---------|-----------------|-------------|
| **Обзор** (главная) | [`DesktopWallpaperPreviewTile`](../../GlassSettings/Controls/DesktopWallpaperPreviewTile.xaml) | только обои 148×86 | `WallpaperPreviewCrossfade`; источник — `DesktopWallpaperImageCache` |
| **Визуальные стили** | [`ThemeHeroPreview`](../../GlassSettings/Controls/ThemeHeroPreview.xaml) | только обои 340×192 | `RefreshPreview` / `ForceRefreshPreview`; chrome overlay не участвует в crossfade |
| **Композиция** | два [`DwmCompositionCascadePreview`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml) | обои + blur + glow (Win7) | sync snapshot; Vista split inactive — отдельная логика tint, обои общие |
| **Стилиизация** | [`StylizationLayeredPreview`](../../GlassSettings/Controls/StylizationLayeredPreview.xaml) | слой 1 обои + слой 2 стекло (blur+glow+tint) | 5 слоёв каскада; reflection/material/atlas — без wallpaper crossfade |
| **Текст и шрифты** | [`TextGlassPreviewHost`](../../GlassSettings/Controls/TextGlassPreviewHost.xaml) → `DwmCompositionCascadePreview` (`PreviewKind=WallpaperBlurBase`) | обои + карточка стекла (blur+glow+tint+ink) | ink-overlay **не** перерисовывается при смене обоев; см. ниже |

### Триггеры

| Событие | Типичный handler |
|---------|------------------|
| `DesktopEnvironmentWatcher.WallpaperChanged` | debounce ~120 ms → refresh preview на странице |
| Cold start | `DesktopWallpaperImageCache.BeginLoad`; `CompositionWallpaperLayersCache.BeginPrewarm` (если OG UI exposed) |
| Invalidate blur cache | смена обоев → invalidate + re-prewarm ([startup doc](GlassSettings-startup-white-flash.md)) |

---

## Специфика: «Текст и шрифты»

Контрол: [`TextGlassPreviewHost`](../../GlassSettings/Controls/TextGlassPreviewHost.xaml.cs) оборачивает `DwmCompositionCascadePreview` с `PreviewKind = WallpaperBlurBase` — каскад **обои → карточка стекла** (blur+glow+tint+ink внутри карточки, inset как на Композиции); `GlassType` выбирает Vista/Win7 state (как слой «Стекло» на Стилизации).

**Ink-overlay** (`TextOverlayA/B` внутри карточки стекла, поверх tint): `TextPreviewNative.PaintToBuffer` → `SetTextOverlay`. Bitmap **не зависит** от обоев (straight-alpha BGRA).

При смене обоев:

- вызывается только [`RefreshBackdropAsync`](../../GlassSettings/Controls/TextGlassPreviewHost.xaml.cs) → `Cascade.RefreshWallpaperBlurFromRegistryAsync()`;
- **не** вызывается `RepaintTextLayerAsync` (устраняет лишний flash swap A↔B);
- [`ApplyPreviewKindChrome`](../../GlassSettings/Controls/DwmCompositionCascadePreview.xaml.cs) **не** скрывает `TextOverlay*` при refresh (раньше давало однократное мерцание до crossfade).

Перерисовка текста — только при смене темы/accent или параметров ink (`ApplyGlowState` / `RepaintTextLayerAsync`).

---

## Специфика: «Стилизация»

Документ: [GlassSettings-stylization-layered-preview.md](GlassSettings-stylization-layered-preview.md).

Кратко:

- **Слой «Стекло»** — не статическая acrylic-кисть: blur + glow (Win7) + tint из [`CompositionPreviewRegistryStates`](../../GlassSettings/Services/CompositionPreviewRegistryStates.cs) + `GlassType`; Vista — active glass color.
- **Слой «Отражение»** — crop 1:1 из верхнего левого угла (`ImageBrush`, `Stretch=None`); default PNG 102 — первая плитка коллекции (как material).
- **Коллекции** — gate OFF удаляет custom + intensity; layout по `maxColumns` для выравнивания с material.

---

## Специфика: «Композиция»

Полный документ: [DwmColorizationPage.md](DwmColorizationPage.md) (секция «Каскадное live-превью»).

- Live drag blur radius/balance: видимый каскад — sync blur; скрытый — `skipBlurRecompute` до commit gesture.
- Win7 glow: `(1 − luminance) × afterglow`, premultiplied BGRA.

---

## Чек-лист регрессии (смена обоев)

1. Первый показ страницы — без fade (instant).
2. Вторая и последующие смены — **500 ms**, обои и blur стартуют **одновременно** (composition, stylization, text).
3. «Текст и шрифты» — текст остаётся видимым на протяжении crossfade фона.
4. «Визуальные стили» / главная карточка — только crossfade обоев, без лишнего repaint.
5. Cold start + переход на «Композиция» — cache hit 316×168 без пустого blur flash.

---

## Файлы (индекс)

| Файл | Роль |
|------|------|
| `Controls/WallpaperPreviewCrossfade.cs` | crossfade двух `Image`, константа 500 ms |
| `Controls/UiElementCrossfade.cs` | crossfade blur/glow rectangles |
| `Controls/DwmCompositionCascadePreview.*` | каскад composition + WallpaperBlurBase (обои → стекло+ink) |
| `Controls/StylizationLayeredPreview.*` | каскад stylization + combined snapshot |
| `Controls/TextGlassPreviewHost.*` | text page wrapper |
| `Controls/ThemeHeroPreview.*` | visual styles hero |
| `Controls/DesktopWallpaperPreviewTile.*` | home wallpaper card |
| `Services/CompositionWallpaperPipelineBuilder.cs` | blur pipeline |
| `Services/CompositionWallpaperLayersCache.cs` | prewarm / cache hit |
| `Services/DesktopWallpaperImageCache.cs` | shared wallpaper decode |
| `Services/CurrentWallpaperResolver.cs` | path + identity |
| `Services/DesktopEnvironmentWatcher.cs` | wallpaper / theme events |
