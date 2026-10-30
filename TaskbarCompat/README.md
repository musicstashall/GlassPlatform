# TaskbarCompat

**Связанный код:** `TaskbarCompat/`  
**Гайд:** [docs/TaskbarBorders.md](docs/TaskbarBorders.md)

## Назначение

`TaskbarCompat.dll` — XAML-патчинг панели задач Win11 в `explorer.exe` (lazy по Tray-аттачу) и смежных окон (выбор раскладки, переполнение области уведомлений, тултипы).

| Surface | HWND / элемент | Статус |
|---------|----------------|--------|
| Taskbar | `Shell_*TrayWnd` island (`BackgroundFill` / `BackgroundStroke`) | активен |
| InputSwitcher | `Shell_InputSwitchTopLevelWindow` → тип `…TextInput.Common.InputSwitcher` | активен (v1) |
| Tray overflow | `TopLevelWindowForOverflowXamlIsland` → `SystemTray.NotificationAreaOverflow` / `Border#OverflowFlyoutBackgroundBorder` | активен (v1) |
| HoverFlyout | `XamlExplorerHostIslandWindow` → `HoverFlyoutBackgroundControl` / `Border#HoverFlyoutBackground` | активен; только XAML |
| Taskbar ToolTip | `Xaml_WindowedPopupClass` → тип `Windows.UI.Xaml.Controls.ToolTip` | активен; R от трея; только XAML |
| Taskbar MenuFlyout | `Xaml_WindowedPopupClass` → тип `MenuFlyoutPresenter` → Border | активен; SizeChanged; только XAML |

Модель как у Windhawk Taskbar Styler: **одна** DLL, один WUX IXDE (`VisualDiagConnection1`). UICompat в explorer не грузится.

## Структура (ключевое)

| Папка / файл | Роль |
|--------------|------|
| `config/TaskbarRegistry.*` | `Tray\` / `Tray\Xaml\*` |
| `xaml/XamlDiagInit.*` | IXDE / TAP schedule |
| `xaml/TaskbarTapSite.*` | TAP site + marshal shutdown/reapply |
| `xaml/TaskbarVisualTreeWatcher.*` | VTC → registries |
| `xaml/TaskbarBackgroundRegistry.*` | фон/бордюр/зоны трея |
| `xaml/InputSwitchPanelRegistry.*` | backdrop/углы/бордюр InputSwitcher |
| `xaml/TrayOverflowPanelRegistry.*` | backdrop/углы/бордюр tray overflow; theme-Stop: Neutral только на Border, Root = ClearValue |
| `xaml/TrayHoverFlyoutPanelRegistry.*` | backdrop/углы/бордюр HoverFlyout (превью приложений) |
| `xaml/TrayTooltipPanelRegistry.*` | backdrop/углы/бордюр taskbar ToolTip; theme-Stop: без SetValue(stale) Background |
| `xaml/TrayMenuFlyoutPanelRegistry.*` | backdrop/углы/бордюр MenuFlyout (Border chrome) |
| `xaml/brushes/*`, `xaml/effects/*` | Backdrop / blur; Neutral = ThemeResource Acrylic (`XamlReader`) |

**Потоки:** apply InputSwitch / Overflow / HoverFlyout / Tooltip / MenuFlyout — sync на VTC UI; reload — `RunAsync` + `weak_ptr`/`shutdown`; restore — heap-payload ≤250 мс. Без `Loaded`/`DispatcherTimer` в чужом XAML.

**Theme → Stop (Overflow / Tooltip):** см. [TaskbarBorders.md](docs/TaskbarBorders.md) — Neutral ThemeResource только на flyout Border / ToolTip Background; не RootGrid; не `SetValue(original)` Background после epoch. Баровый `TaskbarBackgroundRegistry` для этих симптомов не трогать.

## Выгрузка

`TaskbarCompat_Shutdown` → `ShutdownActiveWatcher` → `RestoreAllFills` (tray + InputSwitch + Overflow + HoverFlyout + Tooltip + MenuFlyout) → Unadvise. Без отдельных Wait под смежные панели.
