# Матрица опробованных методов — anim-border DropDown / ViewControl / ComboLBox

Журнал экспериментов по **кастомной рамке flyout во время pop-in** для трёх popup-категорий (не Menu). Post-uncloak и defer-infrastructure затрагиваются, если они были частью той же ветки.

**Эталон (работает):** NativeTheme Menu — border parts в **client-area** (UAH/DTB), попадают в DWM thumbnail при `SOURCECLIENTAREAONLY`.

**Ограничение архитектуры:** DWM thumbnail real hwnd — `SOURCECLIENTAREAONLY = TRUE`; NC GDI на real **не входит** в anim-слой. Real hwnd **cloaked** в ctor `PopupIn`.

**Текущее состояние кода (после отката v3):** backup `2026-06-22_1210` ≈ **v2** (client-edge + `PrimeDeferredFlyoutThumbnail` в mem DC). Post-uncloak pipeline из `2026-06-22_1107` сохранён.

См. также: [FlyoutBorderPaintOrder.md](FlyoutBorderPaintOrder.md).

---

## Условные обозначения

| Символ | Значение |
|--------|----------|
| ✅ | Работает / цель достигнута для данного контрола |
| ⚠️ | Частично, с оговорками |
| ❌ | Не работает или визуально неверно |
| 💥 | Регрессия (мерцание, артефакты, поломка UX) |
| — | Не применялось к этому контролу |
| 🔄 | Откачено |
| 📋 | Задокументировано как anti-pattern, не повторять |

---

## Хронология веток (backup)

| ID | Backup | Суть | Статус |
|----|--------|------|--------|
| A0 | *(до 2133)* | Без defer: `PaintHostBorder` до cloak при anim | 🔄 исходная проблема «пустой оболочки» |
| A1 | `2026-06-21_2133_defer-border-dropdown` | Введён `SetHostBorderDeferred`; без anim-border в WndCompat | 🔄 |
| A2 | `2026-06-21_2141_flyout-anim-border-fix` | Defer + **suppress** custom NC (`DefSubclassProc`) | 🔄 |
| A3 | `2026-06-21_2149_flyout-anim-border-fix` | Defer **без** pre-cloak `PaintHostBorder` (skip paint если anim) | 🔄 база до 2223 |
| A4 | `2026-06-21_2206_flyout-anim-regression-fix` | Pre-cloak `PaintHostBorder` **до** `SetHostBorderDeferred`; black NC suppress при defer | 🔄 |
| A5 | `2026-06-21_2223_dropdown-thumbnail-fix` | + `SyncClientPaintForThumbnail` pre-anim; custom NC ring при defer | 🔄 регрессия контента |
| A6 | `2026-06-21_2235_dropdown-border-scrollbar` | + `PrepareFlyoutThumbnailSource` в `Start()`; `TouchUpHostBorder` post-uncloak; subclass order ListviewPopup | 🔄 |
| A7 | `2026-06-21_2250_revert-to-2149-dropdown` | Откат к A3 | 🔄 |
| A8 | `2026-06-22_1107_dropdown-post-uncloak-fix` | `OnFlyoutAnimationFinished` + `RefreshFlyoutClientAfterUncloak`; purge stale defer HWND | ✅ post-uncloak |
| A9 | `2026-06-22_1125_dropdown-anim-border` | GDI NC ring + `WM_PRINT` `PRF_NONCLIENT` (паритет Immersive menu) | 🔄 v1 anim-border |
| A10 | `2026-06-22_1147_dropdown-anim-border-v2` | Client-edge `PaintFlyoutClientEdgeBorderForThumbnail` + `WM_PRINT` `PRF_CLIENT`; `PrimeDeferredFlyoutThumbnail` | 🔄 **текущий код** |
| A11 | `2026-06-22_1210_dropdown-anim-border-v3` | Backup **до** v3 (состояние A10) | — |
| A12 | v3 *(без отдельного backup после merge)* | Единый TLS, real-surface repaint, per-frame `PrintWindow`, suppress `WM_NCPAINT` | 🔄 откат 22.06 |

*Ветки `2026-06-22_0826` … `1045_immersive-*` — в основном **Menu**; на DropDown не переносились.*

---

## Матрица методов

### 1. Defer и pre-cloak setup (GlassCore)

| Метод | DropDown | ViewControl | ComboLBox | Backup / версия | Итог |
|-------|----------|-------------|-----------|-----------------|------|
| `PaintHostBorder` **до** cloak при `animation.enable` | ⚠️ | ⚠️ | ⚠️ | A0 | Пустая оболочка шире client в anim |
| `SetHostBorderDeferred` **без** pre-cloak paint | ❌ | ❌ | ❌ | A2, A3, A8 | Системная белая/синяя NC во время anim |
| Pre-cloak `PaintHostBorder` → затем `SetHostBorderDeferred` | ⚠️ | ⚠️ | ⚠️ | A4, A6 | Рамка на real корректна, но **не в thumbnail** (NC вне client) |
| Только `ApplyPopupSwcaEffect` + defer, paint post-uncloak | ⚠️ | ⚠️ | ⚠️ | A3, A8 | Post-uncloak ок после A8; during anim — нет кастомной рамки в thumbnail |
| `ApplyBorderEffect` на **real** hwnd в `DropDownPopupIn::SetBackdropEffectForAnimation` (паттерн BWM) | — | — | — | BWM `FlyoutAnimation.cpp` | В Glass **не пробовали**; BWM рисует DWM border на real во время anim |
| Strip DWM border на real + overlay только blur (`m_cornerRadius=0`) | ✅ | ✅ | ✅ | A3+ | Нет «пустой» DComp-оболочки; не решает GDI border в thumbnail |
| `ApplyBorderEffect` / border на `m_backdropWindow` во время anim | 💥 | 💥 | 💥 | ранние попытки | Контур шире client thumbnail | 📋 |
| `DWMWA_BORDER_COLOR` на anim overlay из settings (real не трогать) | ⚠️ | ⚠️ | ⚠️ | A5, A6 | Цвет на overlay, не в client thumbnail |
| `m_cornerRadius` из `border.cornerType` на DComp clip | ⚠️ | ⚠️ | ⚠️ | A4, A6 | Скругление без цветного rim; рамка в thumbnail всё ещё отдельная задача |

### 2. Рамка во время анимации — WndCompat / GDI

| Метод | DropDown | ViewControl | ComboLBox | Backup / версия | Итог |
|-------|----------|-------------|-----------|-----------------|------|
| Suppress custom NC → `DefSubclassProc` при defer | ❌ | ❌ | ⚠️ | A2, A8 (1125 до фикса) | Белая/синяя системная NC; ComboLBox scrollbar ок |
| Black NC fill **без** outline при defer | ❌ | ❌ | ❌ | A4 (2206) | Стирает pre-cloak ring; рамка не видна | 📋 |
| Custom GDI NC ring: `WM_NCPAINT` black fill + `PaintFlyoutNcOutlineGdi` | ⚠️ | ⚠️ | ⚠️ | A5, A6, A9 | На real ок; **не в thumbnail** |
| То же + `WM_PRINT` + `PRF_NONCLIENT` для thumbnail | ❌ | ❌ | ⚠️ | A9, A10 (до client-edge) | NC не попадает в `SOURCECLIENTAREAONLY` |
| **Client-edge** `PaintFlyoutClientEdgeBorderForThumbnail` в `WM_PRINT` + `PRF_CLIENT` | ⚠️ | ⚠️ | ⚠️ | A10 | Логи `painted=1`, но `cornerType=0` (TLS), цвета неверные |
| Client-edge на **real DC** `WM_PAINT` + `GetDC` (fallback) | — | 💥 | — | A10 | ViewControl: **две рамки со смещением**, залипают после anim | 📋 |
| `WM_NCPAINT` при defer: только black fill, **без** `DefSubclassProc` | 💥 | ⚠️ | 💥 | A12 v3 | ComboLBox: **дефолтный scrollbar** | 📋 |
| `WM_NCPAINT` при defer: GDI ring + `DefSubclassProc` (scrollbar) | ⚠️ | ⚠️ | ✅ | A9, A10 | Scrollbar ок; рамка в thumbnail по-прежнему не NC |
| `WM_ERASEBKGND` / `WM_PRINTCLIENT` black `PatBlt` на shell | ✅ | ✅ | — | давно | Убирает белую кайму MonthCal; не anim-border |
| UAH/DTB border в client (как NativeTheme menu) | — | — | — | — | DropDown shell **не получает** UAH; не реализовано |

### 3. Thumbnail / синхронизация client

| Метод | DropDown | ViewControl | ComboLBox | Backup / версия | Итог |
|-------|----------|-------------|-----------|-----------------|------|
| `SyncClientPaintForThumbnail` **до** show / cloak | ⚠️ | ⚠️ | 💥 | A5, A6 | Белые scrollbar / пустой thumbnail / регрессия контента | 🔄 откат |
| `PrepareFlyoutThumbnailSource` в `PopupIn::Start()` после show | ⚠️ | ⚠️ | ⚠️ | A6 | Порядок subclass ListviewPopup важен |
| `PrimeDeferredFlyoutThumbnail`: `WM_PRINT` → **mem DC** + `DwmFlush` в `Start()` | ⚠️ | ⚠️ | ⚠️ | A10 | Один раз; **не обновляет live** DWM thumbnail |
| `PrintWindow(PW_CLIENTONLY)` **один раз** в `Start()` | — | — | — | обсуждалось | Не выделено в backup; эффект сомнителен для anim |
| Per-frame: `GetDC` + client-edge + `PrintWindow` + `DwmFlush` в **Animator** | 💥 | 💥 | 💥 | A12 v3 | **Мерцание каждый кадр** | 🔄 📋 |
| `PrimeFlyoutNcBlackBeforeCloak` (black NC до `CreateDropDownPopupIn`) | ⚠️ | ⚠️ | ⚠️ | A12 v3 | Подавление системной NC; не дало рамку в thumbnail | 🔄 |
| DComp border visual поверх thumbnail (не mutating hwnd) | — | — | — | — | **Не пробовали** (кандидат v4) |
| Снять `SOURCECLIENTAREAONLY` с thumbnail | — | — | — | — | **Не пробовали**; риск misalignment |

### 4. Контекст и TLS

| Метод | Все три | Backup / версия | Итог |
|-------|---------|-----------------|------|
| `inline thread_local g_dropDownCtx` в `DropDownContext.h` (GlassCore + WndCompat static lib) | ❌ | A10 | **Две копии TLS**: WndCompat читает нули → `cornerType=0` |
| Единый `g_dropDownCtx` в `WndCompat/.../DropDownContext.cpp` + `WndCompat_SetDropDownContext` | ⚠️ | A12 v3 | Исправляет TLS; откат вместе с v3 |
| Fresh `QueryBorderContext` в post-uncloak (не stale `g_dropDownCtx`) | ✅ | A6 `TouchUpHostBorder` | Нужно при touch-up; full `PaintHostBorder` — нет |

### 5. Post-uncloak

| Метод | DropDown | ViewControl | ComboLBox | Backup / версия | Итог |
|-------|----------|-------------|-----------|-----------------|------|
| Только `PaintDeferredFlyoutBorderIfNeeded` / `PaintHostBorder(FrameOnly)` | ⚠️ | ⚠️ | ⚠️ | A3 | Контент иногда затирается |
| Full `PaintHostBorder` (с `ApplyBorderEffect` + `WM_NCACTIVATE`) post-uncloak | 💥 | 💥 | 💥 | A5, A6 | Wipe client / flicker | 📋 |
| `TouchUpHostBorder` (GDI ring, без DWM refresh) | ⚠️ | ⚠️ | ⚠️ | A6 | Меньше wipe; заменён pipeline A8 |
| `OnFlyoutAnimationFinished`: defer border → **`RefreshFlyoutClientAfterUncloak` всегда** → clear defer | ✅ | ✅ | ✅ | A8 | Пользователь подтвердил устранение post-uncloak бага |
| `PostMessage(WM_GLASS_PAINT_FLYOUT_HOST_BORDER)` после uncloak | 💥 | 💥 | 💥 | ранние menu-ветки | Flicker | 📋 |
| `RefreshFlyoutFrameAfterUncloak` с paint из worker `Finish()` | 💥 | 💥 | 💥 | до A8 | Flicker / wrong thread | 📋 |
| `PurgeDeadHostBorderDeferredHwnds` при reuse HWND | ✅ | ✅ | ✅ | A8 | Инфраструктура defer-set |

---

## Сводка по контролам (during anim — рамка в thumbnail)

| Контрол | Лучший из опробованного | Типичный провал | Открыто |
|---------|-------------------------|-----------------|--------|
| **DropDown** (ListviewPopup shell) | Client-edge v2 теоретически верный путь | NC GDI (v1); TLS `cornerType=0`; белая NC при suppress | TLS fix + live thumbnail update без per-frame repaint |
| **ViewControl** | То же + **без** `WM_PAINT`/`GetDC` fallback | Две залипшие рамки (v2+v3) | Как DropDown; Toolbar через PaintDispatcher в sync |
| **ComboLBox** | `WM_NCPAINT`: ring + `DefSubclassProc` | v3 без `DefSubclassProc` → дефолтный scrollbar; мерцание v3 | Client-edge только в capture-path; scrollbar обязателен |

---

## Симптомы ↔ методы (быстрый поиск)

| Симптом | Вероятная причина | Версия |
|---------|-------------------|--------|
| Пустая оболочка шире контента | `PaintHostBorder` / DWM border до cloak или на backdrop | A0 |
| Белая/синяя NC во время anim | `DefSubclassProc(WM_NCPAINT)` при defer без custom ring | A2, A3 |
| Рамка на real, пусто в anim | NC вне `SOURCECLIENTAREAONLY` | A4–A9 |
| Неверный corner/цвет рамки | Двойной TLS `g_dropDownCtx` | A10 |
| Мерцание каждый кадр | Per-frame `PrintWindow` в Animator | A12 v3 |
| Дефолтный scrollbar ComboLBox | `WM_NCPAINT` без `DefSubclassProc` | A12 v3 |
| Две рамки ViewControl после anim | Client-edge на persistent real DC | A10 |
| Контент стёрт после anim | Нет `RefreshFlyoutClientAfterUncloak` | до A8 |
| Контент стёрт сразу после anim | Full `PaintHostBorder` post-uncloak | A5, A6 |

---

## Кандидаты (не опробованы на DropDown-категории)

1. **DComp border ring** вокруг thumbnail visual (цвет/радиус из settings) — без GDI на cloaked hwnd.
2. **Единый TLS** + client-edge **только** в `WM_PRINT`/`WM_PRINTCLIENT`, без real-surface fallback.
3. **Один** `PrintWindow` при смене defer→live (uncloak), не в Animator.
4. BWM-путь: `ApplyBorderEffect` на real при anim — осознанный A/B против defer+GDI (в Glass отвергнут из-за empty shell, но в BWM работает иначе).
5. Отключение `SOURCECLIENTAREAONLY` — только с прототипом alignment.

---

## Связанные файлы

| Область | Файлы |
|---------|--------|
| Defer state | `GlassCore/src/Subsystems/FlyoutFrame.cpp` |
| Show / attach | `GlassCore/src/Hooks/DropDownHooks.cpp`, `ComboLBoxHooks.cpp` |
| Anim | `GlassCore/src/Subsystems/Animation.cpp` |
| Subclass | `WndCompat/Popup/DropDown/*.cpp`, `ComboLBox/ComboLBoxSubclass.cpp` |
| GDI helpers | `GlassShared/src/MenuUtils.cpp` |
| Порядок фаз | [FlyoutBorderPaintOrder.md](FlyoutBorderPaintOrder.md) |

---

*Обновлено: 2026-06-22. При новой попытке — добавить строку в матрицу и ID backup в `_backups/`.*
