# Root «Дополнительно»: фон иконок и изображений контролов

Удаление цвета из фона иконок контролов Root-окон — настраиваемый chroma-key,
аналог `Menu\ColorTreatAsTransparent`, но с раздельными ключами на тип контрола.
Menu-ключи на эти поверхности **не** влияют (см. [PopupAdvancedRendering.md](PopupAdvancedRendering.md)).

> Миграция легаси-ключей не выполняется (приложение не было публичным): читается
> только `ColorTreatAsTransparent*`. Menu тоже больше не читает старые
> `EnableIconBackgroundColorRemoval` / `IconBackgroundColor*`.

## Реестр

База: `HKCU\Software\StanislavKa\GlassPlatform`. Ключи глобальные — не зависят от `Root\Active` / `Root\Inactive`.

| Подключ | Value | Семантика |
|---------|-------|-----------|
| `Root\Control\Ribbon` | `ColorTreatAsTransparent` (DWORD AARRGGBB) | presence = enable; дефолт при записи `0xFFFFFFFF` |
| | `ColorTreatAsTransparentThreshold` (DWORD 0–510) | дефолт 50 (при дефолте value удаляется) |
| `Root\Control\ListView` | то же | то же |
| `Root\Control\TreeView` | то же | то же |
| `Root\Control\Toolbar` | то же | то же (`ToolbarWindow32`) |
| `Root\Control\Button` | то же | то же (кнопки без `BS_ICON/BS_BITMAP`, BitBlt-путь) |
| `Root\Control\Static` | то же | то же (Static c изображением; **дефолт не удаляет цвет**) |

## Runtime (GlassCore + WndCompat)

Чтение — `Settings::QueryIconBackgroundColorRemovalContext({ L"Root", L"Control", L"{Kind}" }, …)`
один раз на входе внешней paint-сессии (не на каждый Blt). Для Static — `Shared::RegHelper`
внутри субкласса при `STM_SETIMAGE`.

| Поверхность | Файл | Поведение без ключа | С ключом |
|-------------|------|---------------------|----------|
| Ribbon (24 bpp path) | `UiRibbonHooks.cpp` | только `Promise32BPP` (фон сохраняется) | `PrepareBitmapForAlphaBlend` с `chromaKey` |
| ListView (BitBlt/StretchBlt non-alpha) | `ListViewHooks.cpp` | black-key (чёрный фон ImageList → прозрачный) | `SpriteEffect` (настроенный chroma) |
| TreeView (MaskBlt non-alpha) | `TreeViewHooks.cpp` | black-key | `SpriteEffect` |
| Toolbar (MaskBlt/BitBlt non-alpha) | `ToolbarHooks.cpp` | black-key | `SpriteEffect` |
| Button (comctl32 BitBlt non-alpha) | `ButtonHooks.cpp` | black-key | `SpriteEffect` |
| MFC Afx toolbar (16×16) | `RootGdiHooks.cpp` | black-key (без настройки/UI) | — |
| Static с изображением | `WndCompat/Controls/StaticSubclass.cpp` | только `Promise32BPP` (alpha=255, цвет не удаляется) | `SpriteEffect` |

Общий код GlassCore-хуков: `GdiUtils::ApplyBlackKeyAlpha` / `GdiUtils::ApplyIconBackgroundAlpha`
(`GlassCore/src/Subsystems/GdiHookUtils.cpp`). 32 bpp иконки с нативной альфой не трогаются.

### Static (`STM_SETIMAGE`)

Субкласс `StaticImageSubclassProc` вешается **только** на Static со стилем изображения
(`SS_BITMAP` / `SS_ICON`, см. `StaticHasImageStyle`) — в дополнение к mark-only `Static_Attach`.
Перехватывается `STM_SETIMAGE` (только `IMAGE_BITMAP`): битмап без альфы конвертируется
в 32bpp (`Promise32BPP`) — иначе на стекле картинка рисуется некорректно (аналог BWM
`GdipCreateBitmapAlpha`, но без GDI+). Если задан `Root\Control\Static\ColorTreatAsTransparent`,
дополнительно применяется chroma-key. Уже установленное изображение (из ресурса) конвертируется
при attach через `STM_GETIMAGE` + повторный `STM_SETIMAGE` (guard от рекурсии).

Cross-thread detach — `WM_GLASS_DETACH_STATIC` + `SendMessageTimeoutW` (как Rebar).
Сконвертированный битмап живёт вместе с контролом и не отслеживается/не удаляется
(BWM-паритет; прежний хендл контрол возвращает вызывающему).

## UI (GlassSettings, RootWindowsPage)

`SettingsDividerExpander` «Дополнительно» → `Pivot`
Ribbon | ListView | TreeView | Toolbar | Button | Static →
единый `SettingsSystemGateExpander` (picker RGB без альфы + NumberBox threshold).
Вкладка задаёт подключ `Root\Control\{Kind}` и текст-призыв панели; picker/threshold один набор.

- Шапка спойлера в подстроке — описание **активной вкладки** (`*_Subtitle`);
  заголовок на развороте (описание панели) — призыв (`*_Desc`).
- Gate ON — запись только при явном выборе цвета (armed-состояние, как Menu icon background).
- Gate «залипает»: включённый пользователем gate и развёрнутый спойлер **сохраняются**
  при перелистывании вкладок (не сбрасываются/не сворачиваются). Gate OFF — явное действие.
- Gate OFF / «По умолчанию» — удаление `ColorTreatAsTransparent*` активной вкладки.
- Unsaved: точка на вкладке pivot per-kind (`SettingsCommitScope.ForRootControlIcon`),
  индикатор на divider — по любой вкладке (`ForRootControlIcons`).
- Live reload: `RootWindowsLiveReload.NotifyControlIconChanged()` — repaint-only
  (для Static применяется к новым `STM_SETIMAGE`, уже нарисованные картинки не переконвертируются).

Registry-helper: `GlassSettings/Core/Root/RootControlIconRegistry.cs`;
параметры включены в область Root (`CustomizationCatalog.RootParameters`).

## WM_CTLCOLORSTATIC (прозрачный фон Static)

Когда Static с `SS_BITMAP`/`SS_ICON` или с `TRANSPARENT` лежит поверх изображения,
жёсткая `BLACK_BRUSH` в `RootSubclass` закрашивает фон непрозрачным пятном.

**Решение (донор BWM):** для `WM_CTLCOLORSTATIC` и `WM_CTLCOLORLISTBOX` (кроме ComboLBox)
`RootSubclass` вызывает `DefSubclassProc` под scoped `SetBkColor` probe
(`GlassCore_SetCtlColorBkColorProbeAttach` → `CtlColorProbe.cpp`) и разбирает `LOGBRUSH`:

- кастомный solid (не sys) → вернуть кисть диалога;
- `BS_NULL` + `TRANSPARENT` → вернуть (прозрачный Static);
- иначе → `BLACK_BRUSH` для стекла.

Chroma на bitmap (`StaticSubclass`) — отдельный путь; CTLCOLOR нужен, когда parent
получает `WM_CTLCOLOR*` от Static, а не когда субкласс перехватывает `STM_SETIMAGE`.

## Риски

- Белый chroma на ListView/TreeView/Toolbar/Button может «съесть» светлые детали
  shell-иконок — поэтому дефолт (без ключа) остаётся black-key, chroma только при явной настройке.
- Static: chroma по фотографии уберёт совпадающий цвет по всему изображению — по умолчанию
  ключ выключен (только promotion в 32bpp), включать осознанно.
- Порог: `SpriteEffect` сравнивает каждый канал RGB (`|канал − референс| <= threshold`, BYTE);
  UI допускает 0–510 как в Menu, но эффективно применяется младший байт значения.
