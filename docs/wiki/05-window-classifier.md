# 05 · `GlassAttachGate`, 6 ролей `WindowClassifier` и 21 + 4 обработчика

`WndCompat.dll` классифицирует и обрабатывает окна Win32, WinForms, ATL/WTL и
XAML-мосты. Первичный фильтр `GlassAttachGate::ShouldAttachGlassToHwnd` отсекает
освобождённые от стекла поддеревья (`IsHwndBackdropExempt` / `IsSubtreeGlassDisabled`),
после чего 8-шаговый `WindowClassifier::Classify` распределяет `HWND` по 6 ролям
`WindowRole` и выбирает стратегию крепления: `*Subclass` (`SetWindowSubclass`) или
`*Host` (без сабклассинга).

```mermaid
flowchart TD
  ENTRY["WinEvent + Initial Scan<br/>EVENT_OBJECT_CREATE · SHOW<br/>WM_GLASS_ATTACH_ROOT (0x201)"] --> GATE{"GlassAttachGate::<br/>ShouldAttachGlassToHwnd<br/>carve-out: ComboLBox · NC CoreWnd · HostPolicy"}
  GATE -->|"Exempt / Subtree disabled"| SKIP["Skip Attach<br/>MarkSubtreeGlassDisabled(hRoot)"]
  GATE -->|"Allow"| NORM["WindowClassifier::Classify<br/>NormalizeWindowClass → Steps 0..7"]
  GATE -->|"CoreWindow / ControlCenter"| SPECIAL
  NORM --> ROOT["Root · Top-Level<br/>GA_ROOT · CabinetWClass / ExploreWClass"]
  NORM --> CONTROL["Control (21)<br/>WS_CHILD · WndCompat/Controls/"]
  NORM --> POPUP["Popup (4 семейства)<br/>Menu #32768 · Tooltip/KbxLabel<br/>ComboLBox · DropDown"]
  NORM --> SPECIAL["Special<br/>CoreWindow / WinUI / MSHTML"]
  NORM --> TRAY["Tray<br/>Shell_TrayWnd / SecondaryTray"]
  NORM --> IGN["Ignored<br/>HardBlock · ATL:HEX · Progman subtree"]
  SKIP --> IGN

  TRAY -.->|"WM_GLASS_ENSURE_TASKBAR_COMPAT (0xB02)"| TB["TaskbarCompat"]
  SPECIAL -.-> UI["UICompat (tid-scoped IXDE)"]

  classDef role fill:#1f6feb1a,stroke:#1f6feb;
  class ROOT,CONTROL,POPUP,SPECIAL,TRAY,IGN role;
```

## Шаги `WindowClassifier::Classify`

| Шаг | Проверка | Результат |
|---|---|---|
| 0 | `CoreWindow` / `ControlCenter` | `Special` (UWP-хосты) |
| 1 | `IsHardBlockedClass` · `ATL:HEX` · HostPolicy | `Ignored` |
| 2 | `Shell_TrayWnd` / `Progman` subtree | `Tray` |
| 3 | Popup: `#32768`, Tooltip | `Popup` |
| 4 | `Special` · `NetUiToolWindow` | `Special` |
| 5 | `WS_CHILD` | `Control` (21) |
| 6 | Soft-blocked (`GA_ROOT` есть) | `Control`, стратегия `*Host` |
| 7 | `GA_ROOT` | Explorer / Native top-level |

Выход: `{ WindowRole, ControlKind }`; далее `ControlKind` уточняется до 11 вариантов
`WinForms_*` и 4 `Atl/Wtl`.

## Стратегии крепления

- **18 `*Subclass`** — `SetWindowSubclass` на родном UI-потоке.
- **5 `*Host`** без сабклассинга: `ScrollBar` · `TrackBar` · `UpDown` · IE · Tray.

Выгрузка (2026-09): `PostMessage(WM_GLASS_DETACH_*)` → `RemoveWindowSubclass` на родном
потоке → `Wait(INFINITE)` → `g_hookInstalled = false` → `ScanStaleCallbacksBeforeFree` (H91).
`SendMessage` при отвязке запрещён — риск deadlock.

**SAFETY.** `g_hookInstalled` сбрасывается **только после** `WaitForSingleObject(INFINITE)`,
а `GlassCompat` перед `FreeLibrary` запускает проверку «зависших» колбэков.

Источник: [docs.html · раздел 05](https://glass-platform.ru/docs.html) · ← [04](04-gdi-32bpp) · → [06](06-win11-xaml)
