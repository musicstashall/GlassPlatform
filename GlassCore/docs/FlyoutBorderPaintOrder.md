# Порядок отрисовки NC-рамки flyout (Menu / DropDown / ComboLBox / ViewControl)

Документ фиксирует **обязательный** порядок для popup с flyout-анимацией (`PopupIn` / `MenuPopupIn` / `DropDownPopupIn`). Нарушение приводит к «пустой рамке» во время pop-in и/или post-uncloak flicker.

## Почему defer нужен

Анимация строится из слоёв:

1. **Real hwnd** — SWCA/backdrop на окне меню или DropDown.
2. **m_backdropWindow** — glass-fill на полный rect (DWM corners + border).
3. **DWM thumbnail** — `SOURCECLIENTAREAONLY = TRUE` для контента.

Если NC-рамку нарисовать на real **до cloak**, она не попадает в thumbnail, а backdrop уже показывает контур на весь rect → видна «пустая оболочка» шире контента.

**Решение:** defer блокирует **повторный** `PaintHostBorder` / `ApplyBorderEffect` в GlassCore до uncloak; кастомная GDI-рамка на real hwnd рисуется в WndCompat (как у Immersive menu). Backdrop overlay — только blur/glass **без** DWM border/corners.

## Общий state (GlassCore)

Per-popup **HWND** (не thread-local — submenu / cascade):

| API | Файл |
|-----|------|
| `FlyoutFrame::SetHostBorderDeferred` | `GlassCore/src/Subsystems/FlyoutFrame.cpp` |
| `FlyoutFrame::IsHostBorderDeferred` | |
| `FlyoutFrame::ClearHostBorderDeferred` | |
| `FlyoutFrame::PaintDeferredHostBorder` | post-uncloak для DropDown-пути |
| `Menu::SetFlyoutBorderDeferred` … | thin-обёртки над FlyoutFrame (`MenuEvents.cpp`) |

Провайдеры WndCompat: `MenuEventProvider::IsFlyoutBorderDeferred`, `DropDownEventProvider::IsFlyoutBorderDeferred` → `FlyoutFrame::IsHostBorderDeferred`.

## Фаза 1 — show / attach (до cloak)

```
ApplyPopupSwcaEffect(real hwnd)     // SWCA/DWM на real — всегда
if (animation будет):
    SetHostBorderDeferred(hwnd, true)
else:
    PaintHostBorder(hwnd, …)          // сразу, без defer
Create*PopupIn(...)                   // cloak real в ctor PopupIn
```

| Тип | Точка входа | Условие defer |
|-----|-------------|---------------|
| Menu | `MenuEvents::OnPopupSized` | `FlyoutAnimationContext.enable` (часть `"Menu"`) |
| DropDown / ListviewPopup | `DropDownHooks::ApplyDropDownFlyoutAtTarget` | `ctx.animation.enable` |
| ViewControl | `DropDownHooks::OnViewControlAttach` | `ctx.animation.enable` |
| ComboLBox | `ComboLBoxHooks::OnComboLBoxShow` | `schedulePopupIn` (Glass anim **или** SPI combo anim) |

**Не вызывать** `PaintHostBorder` / `OnPaintFlyoutHostBorder` на real, если defer активен.

## Фаза 2 — во время анимации

### Почему NC GDI не попадает в thumbnail

DWM thumbnail real hwnd: `SOURCECLIENTAREAONLY = TRUE` (`Animation.cpp`). Real flyout **cloaked** в ctor `PopupIn`. `WM_PRINT` с `PRF_NONCLIENT` рисует вне client-area → **не видно** в DComp-слое.

Эталон **NativeTheme menu**: при defer `OnPaintFlyoutHostBorder` заблокирован; рамка во время anim идёт из **UAH/DTB** (`partId=13/27`) — отрисовка border parts **в client-area**, попадающая в thumbnail.

DropDown shell UAH не получает → аналог: **client-edge** через `WM_PRINT` + `PRF_CLIENT`.

### Backdrop overlay (`*PopupIn::SetBackdropEffectForAnimation`)

- `m_backdropWindow`: только `ApplyBackdropEffect` (без `ApplyBorderEffect`, без border в `ApplyPopupSwcaEffect`).
- Animation `window`: `DWMWCP_DONOTROUND` + `DWMWA_BORDER_COLOR = NONE` (Win11+).
- `m_cornerRadius = 0` (без скруглённого DComp-контура).

### GlassCore — prime thumbnail

В `PopupIn::Start()` после show overlay, **до** `dcompDevice->Commit()`:

- если `FlyoutFrame::IsHostBorderDeferred(m_flyoutHwnd)` → `SendMessageW(WM_PRINT, hdcMem, PRF_CLIENT | PRF_CHILDREN)` + `DwmFlush()`.

Гарантирует первый кадр thumbnail с client-edge рамкой.

### WndCompat — client-edge при defer / NC ring вне defer

| Слой | defer | Путь |
|------|-------|------|
| **client-edge** (thumbnail) | да | `WM_PRINT` + `PRF_CLIENT` → `PaintFlyoutClientEdgeBorderForThumbnail` |
| **NC GDI ring** (post-uncloak / без anim) | нет | `WM_NCPAINT` / `WM_PRINT` + `PRF_NONCLIENT` |

| Модуль | Поведение при defer |
|--------|---------------------|
| Menu Immersive / TraverseLog | `WM_NCPAINT` + `WM_PRINT` (`PRF_NONCLIENT`) → black NC fill + outline |
| Menu Classic / NativeTheme | suppress custom NC → `DefSubclassProc`; border из UAH/DTB в client |
| DropDown | `WM_PRINT` (`PRF_CLIENT`) → client-edge; `WM_NCPAINT` NC ring (не в thumbnail) |
| ViewControl | то же + `WM_PAINT` client-edge fallback (`GetDC`) |
| ComboLBox | `WM_PRINT` (`PRF_CLIENT`) → client-edge; `WM_NCPAINT` NC ring + scrollbar |

Outline: `PaintFlyoutClientEdgeBorderForThumbnail` / `PaintFlyoutNcOutlineGdi` / `HandlePopupMenuNonClientBorderColors` по `g_dropDownCtx.border`.

### Guard в GlassCore

- `FlyoutFrame::PaintHostBorder` — return, если defer.
- `MenuEvents::OnPaintFlyoutHostBorder` — return, если defer.

## Фаза 3 — post-uncloak (GUI-поток flyout)

В `PopupIn::SubclassProc` на `AnimationFinishedMsg` (**до** `RemoveWindowSubclass`):

```cpp
Menu::PaintDeferredFlyoutBorderIfNeeded(hWnd);      // NativeTheme + FlyoutFrame
DropDown::OnFlyoutAnimationFinished(hWnd);          // defer border + client refresh
```

`OnFlyoutAnimationFinished`: `PaintDeferredFlyoutBorderIfNeeded` (если defer ещё активен) → **`RefreshFlyoutClientAfterUncloak` всегда** → `ClearHostBorderDeferred`.

- **Sync** на потоке flyout (`SendMessageTimeoutW` из `~PopupIn`).
- **Не** `PostMessage(WM_GLASS_PAINT_FLYOUT_HOST_BORDER)` — регресс flicker (фаза 2 anti-flicker).
- `MenuPopupIn::RefreshFlyoutFrameAfterUncloak` и `DropDownPopupIn::RefreshFlyoutFrameAfterUncloak` — **no-op**; не рисовать рамку из worker `Finish()`.

Post-uncloak flush:

- Menu Classic → `FlyoutFrame::PaintHostBorder(..., FrameOnly)`.
- Menu NativeTheme → `EnsureNativeThemeFlyoutBorder` + `PaintNativeThemePopupNcBorder` (без `RDW_INVALIDATE` client).
- DropDown / ComboLBox / ViewControl → `PaintHostBorder(..., FrameOnly)` + `RefreshFlyoutClientAfterUncloak`.

## Cleanup

| Событие | Действие |
|---------|----------|
| `MenuEvents::OnPopupDestroyed` | `ClearHostBorderDeferred` |
| `ComboLBoxHooks::OnComboLBoxDetach` | `ClearHostBorderDeferred` |
| DropDown / ViewControl `WM_NCDESTROY` | `ClearFlyoutBorderDeferred` через provider |

## Anti-patterns (регрессии)

| Нельзя | Почему |
|--------|--------|
| `PaintHostBorder` до cloak при включённой анимации | пустая рамка в thumbnail/backdrop |
| Black NC fill без outline при defer (только `PatBlt`) | рамка не видна во время anim; регрессия 2206 |
| `ApplyBorderEffect` / border на `m_backdropWindow` во время anim | контур шире client thumbnail |
| `RefreshFlyoutFrameAfterUncloak` с `Full` flush из worker | post-uncloak «пинок» / flicker |
| `PostMessage(WM_GLASS…)` после uncloak для рамки | дубли + invalidate client |
| `RemoveWindowSubclass` до post-uncloak paint | paint не на том потоке / пропуск рамки |

## Логи (DBG_IMPORTANT)

- `HostBorderDeferred hwnd=…` — defer установлен.
- `PaintDeferredHostBorder` / `PaintDeferredFlyoutBorderIfNeeded` — **после** uncloak.
- `RefreshFlyoutClientAfterUncloak` — post-uncloak client repaint (обязательно).
- `PaintHostBorder … flush=1` (`FrameOnly`) — один раз post-uncloak, не до `Create*PopupIn`.
- `PaintFlyoutClientEdgeBorderForThumbnail hwnd=… defer=1` — client-edge во время anim.
- `PrimeDeferredFlyoutThumbnail hwnd=…` — prime WM_PRINT в `PopupIn::Start`.

## Связанные файлы

- [DropDownAnimBorderMatrix.md](DropDownAnimBorderMatrix.md) — матрица опробованных методов anim-border (DropDown / ViewControl / ComboLBox)
- `GlassCore/src/Subsystems/FlyoutFrame.cpp`
- `GlassCore/src/Subsystems/MenuEvents.cpp`
- `GlassCore/src/Subsystems/Animation.cpp` (`MenuPopupIn`, `DropDownPopupIn`)
- `GlassCore/src/Hooks/DropDownHooks.cpp`, `ComboLBoxHooks.cpp`
- `WndCompat/Popup/Menu/MenuSubclass.cpp`
- `GlassShared/src/MenuUtils.cpp` (`PaintFlyoutClientEdgeBorderForThumbnail`)
- `WndCompat/Popup/DropDown/DropDownSubclass.cpp`, `ViewControlSubclass.cpp`
- `WndCompat/Popup/ComboLBox/ComboLBoxSubclass.cpp`
