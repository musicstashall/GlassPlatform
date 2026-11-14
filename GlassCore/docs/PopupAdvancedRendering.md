# Popup Hub — Menu / Tooltip «Дополнительно»

Краткий справочник по custom rendering в **PopupHub** (Раздельные настройки → Menu / Tooltip).

## Menu General

**UI:** `PopupHubPage` → `MenuGeneralSettingsExpander` (только Menu, раздельные). Подзаголовок шапки: «Дополнительные технические настройки»; на развороте — единый `SettingsColorPickerPanel` (описание, picker RGB без альфа, footer: порог → UWP → без системной тени → современный стиль).

**Registry helper:** `GlassSettings/Core/Menu/MenuRootRegistry.cs`.

| Ключ | UI | Runtime |
|------|-----|---------|
| `Menu\NoSystemDropShadow` | Toggle «Без системной тени» | `MenuSubclass.cpp` |
| `Menu\EnableImmersiveStyle` | Expander gate + toggle «Современный стиль меню» | `MenuSubclass.cpp` |
| `Menu\NoModernAppBackgroundColor` | Toggle в footer | `GlassCore/Hooks/MenuHooks.cpp` |
| `Menu\ColorTreatAsTransparent` | Color picker RGB (без альфа-слайдера) | `QueryMenuIconBackgroundColorRemovalContext` → `SpriteEffect` (сравнение RGB, альфа не участвует) |
| `Menu\ColorTreatAsTransparentThreshold` | NumberBox 0–510 | то же |

**Icon background:** без отдельного gate; запись только при явном выборе цвета или изменении threshold при уже существующем ключе. «По умолчанию» — удаление `ColorTreatAsTransparent*`.

**Область действия:** `Menu\ColorTreatAsTransparent*` влияет **только** на иконки popup-меню. Ribbon / ListView / TreeView управляются отдельными ключами `Root\Control\{Kind}\ColorTreatAsTransparent*` (страница Root-окна → «Дополнительно») — см. [RootAdvancedIconBackground.md](RootAdvancedIconBackground.md).

Все записи PopupHub — `liveFlyout: false` (без `ScheduleFlyoutReload` / live-preview).

Shared flyout (Effect, Gradient, Border, Corners, Animation) — см. TF Config General; не дублируются здесь.

## Menu Advanced

**UI:** `GlassSettings/Pages/Glass/PopupHubPage` → `MenuAdvancedExpander` → pivot Hot / DisabledHot / Focusing / Separator.

**Реестр:** `HKCU\Software\StanislavKa\GlassPlatform\Menu\...` (схема как TF/BWM).

| Ключ | Назначение |
|------|------------|
| `Menu\EnableCustomRendering` | Master-switch (default absent = 0 в Core) |
| `Menu\{Kind}\*` | Per-kind: Disabled, colors, CornerRadius, Width, EnableThemeColorization, ThemeColorizationType |

### Gate (глобальный)

- **ON** на любой вкладке pivot → сразу `Menu\EnableCustomRendering = 1` (`liveFlyout: false`).
- **OFF** на любой вкладке → если в **baseline-снимке** нет сохранённых параметров **других** kind (кроме текущего pivot) — молча удалить subkey текущего kind + master; иначе confirm → wipe **всех** kind-subkeys + master. Решение по снимку, не по live-реестру.
- **«По умолчанию»** → `DeleteKindSubKey` текущего kind; gate и master **не** трогаются.

Gate OFF / wipe удаляет **подключ** `Menu\{Kind}` целиком (`DeleteSubKeyTree`), не только values — включая REG_SZ `ThemeColorizationType`.

Точка на **всех** вкладках pivot: unsaved `EnableCustomRendering` (master) **или** unsaved параметров соответствующего kind.

Координатор: `GlassSettings/Core/Menu/MenuCustomRenderingCoordinator.cs`.

### Runtime (GlassCore)

- `QueryMenuCustomRenderingContext` — паритет BWM/TF v3.1.1 (`Settings.cpp`).
- `MenuRendering.cpp` — MPI_HOT: custom D2D, затем system DTB fallback.
- Separator: D2D-линия при custom; при `Separator\Disabled=1` только BLACKNESS (без system); иначе system fallback (BWM).
- `Separator\CornerRadius` — **не** читается runtime (no-op); в UI скрыт.

## Tooltip Advanced

**UI:** `TooltipAdvancedExpander` — **Text Color** (`DarkMode_Color` / `LightMode_Color`, RGB-only в runtime) + margins. Заливка градиентом — в основном блоке «Цвет градиента», не здесь.

**Реестр:** `ToolTip\DarkMode_Color`, `LightMode_Color`, `MarginsType`, `Margins_*` (Core: `Tooltip\`).

Runtime: `QueryTooltipRenderingContext` → `ARGBToCOLORREF` (альфа не используется) → `SetTextColor` в `TooltipHooks.cpp`; margins в `GetThemeMargins`. В UI пикер без слайдера прозрачности.

Gate OFF: без диалога; при наличии подключей — удаление всех `ToolTip\*` и sync baseline (без live-reload).

Registry helper: `GlassSettings/Core/Tooltip/TooltipRenderingRegistry.cs`.

## Эталоны

- TF Config: TranslucentFlyoutsConfig → Menu / Tooltip tabs.
- Runtime: TranslucentFlyouts v3.1.1 / BWM `ApiEx.cpp`, `MenuRendering.cpp`.
