# GlassPlatform · техническая документация (черновик wiki)

> Черновик страницы для GitHub Wiki. Диаграммы ниже — рабочий пример переноса
> интерактивных схем с [glass-platform.ru/docs.html](https://glass-platform.ru/docs.html)
> на встроенный в GitHub Mermaid. Публикация: см. [README](README.md).

## 1 · Обзор 13 модулей и граф границ доверия

Диаграмма зон повторяет блок `ARCHITECTURE` сайта: переключатели плоскостей
(Control Plane / Win32 / Win11 IXDE) заменены тремя подграфами `subgraph`.

```mermaid
flowchart TB
  subgraph A["ZONE A · Control Plane & Hosting (admin / user session)"]
    SET["GlassSettings<br/>.NET 8 · WinUI 3 UI"]
    HOST["GlassHost<br/>Rundll32 x64 + x86 · HostAlive"]
    EXT["GlassShellExt<br/>COM · MSIX Sparse"]
    TOOLS["LogViewer · GlassSetup<br/>Named Pipe · WiX 5 MSI"]
  end

  subgraph B["ZONE B · Win32 Process Gateway & Hook Engine"]
    COMPAT["GlassCompat.dll<br/>Gate R7 · ExecuteShutdown"]
    CORE["GlassCore.dll<br/>33 Hooks · PaintDispatcher · GVT1"]
    WND["WndCompat.dll<br/>21 Controls · 4 Popups"]
  end

  subgraph C1["ZONE C1 · Explorer WinUI 3 (Zone 3/4 IXDE)"]
    EXP["ExplorerCompat.dll"]
    TASKBAR["TaskbarCompat.dll<br/>6 Surfaces · BorderDrawMode"]
  end

  subgraph C2["ZONE C2 · AppContainer sandbox (no GlassCore)"]
    UI["UICompat.dll<br/>SEH · tid · JoinAll(8000)"]
    WNDC["WndCompat.dll<br/>без GDI-хуков"]
  end

  SHARED["GlassShared (static lib)<br/>GlassMessages 0x200..0xD00 · InjectProcessMap 176 B<br/>TrayPolicy 108 B · GlassStop · 32bpp GDI"]

  HOST --> COMPAT
  SET --> HOST
  EXT --> COMPAT
  COMPAT --> CORE
  COMPAT --> WND
  COMPAT --> EXP
  COMPAT --> TASKBAR
  COMPAT --> UI
  COMPAT --> WNDC
  CORE --- SHARED
  WND --- SHARED
  EXP --- SHARED
  TASKBAR --- SHARED
  UI --- SHARED
```

## 2 · Вход в процесс, Gate R7 и CoreWindow-ветвление

Блок `FLOWCHART` сайта (`GlassCompat::DoInitialize()`). Фильтры по веткам
(«Все ветки / Обычный Win32 / CoreWindow / Stop») в wiki невозможны — вместо них
отдельные свёрнутые блоки `<details>` или подсветка роли цветом узла.

```mermaid
flowchart TD
  A["DllMain → Init"] --> B["InitWorker"]
  B --> C{"Gate R7<br/>HostAlive + GlassStop?"}
  C -->|"Да (R7)"| STOP["Stop / Dead<br/>Abort & Free"]
  C -->|Нет| D{"Allow List?<br/>64 attach / 21 block / 55 built-in"}
  D -->|Нет| STOP
  D -->|Да| E{"CoreWindow?<br/>UWP / XAML"}
  E -->|"Да (UWP)"| F["UICompat + Eager WndCompat<br/>без GlassCore и GDI-хуков"]
  F --> W["WaitExit"]
  E -->|"Нет → обычное HWND"| G{"explorer 11?<br/>IXDE trigger"}
  G -->|"Да (11)"| H["ExplorerCompat + TaskbarCompat<br/>Win11 IXDE Lazy"]
  G -->|Нет| I["GlassCore 33 Hooks<br/>+ WndCompat 21 Controls"]
  I --> J["Paint & Subclassing"]
  H --> W

  classDef gate fill:#1f6feb22,stroke:#1f6feb,color:#1f6feb;
  classDef abort fill:#f8514922,stroke:#f85149,color:#f85149;
  class C,D,E,G gate;
  class STOP abort;
```

## 3 · Конвейер 32bpp и капсула GVT1

Блок `DATA FLOW`: 24/32bpp → DIB → `GlobalTextGlow` → `GdiAlphaBlend` → DWM.

```mermaid
flowchart LR
  T["GDI Text Call<br/>ExtTextOut · DrawText"] --> Q{"32bpp?<br/>Glass DC"}
  Q -->|"Нет"| P["Native GDI<br/>zero-overhead pass-through"]
  Q -->|"Да"| DIB["GVT1 Alpha Capsule<br/>32bpp DIB · Ink Alpha"]
  DIB --> CLR["ClearType isolation"]
  CLR --> GLOW["GlobalTextGlow<br/>гало-подложка"]
  GLOW --> BLEND["GdiAlphaBlend<br/>AC_SRC_ALPHA"]
  BLEND --> DWM["DWM Composition<br/>premultiplied ARGB"]
```

## 4 · Чистая выгрузка: `ExecuteShutdown()` (S0 + фазы 1–6)

Блок `SEQUENCE` с навигацией по фазам. В Mermaid порядок фаз передаётся
`Note over`, а межмодульные барьеры (`WaitForSingleObject(INFINITE)`) — пунктирными
возвратами `-->>`.

```mermaid
sequenceDiagram
  autonumber
  participant GH as GlassHost
  participant GC as GlassCompat
  participant UI as UICompat
  participant TB as TaskbarCompat
  participant EX as ExplorerCompat
  participant CO as GlassCore
  participant WN as WndCompat
  participant IM as InjectMap

  GH->>GC: StopEvent / UnloadRequest.<pid>
  Note over GC: S0 · State::Unloading · DiagLoader = false
  GC->>CO: GlassCore_BeginExplorerShellChromeRestore()
  GC->>IM: TrySetShutdownPhase(UiDetaching = 3)
  GC->>UI: WM_GLASS_UI_XAML_SHUTDOWN (0xC01)
  UI->>UI: COM drain obj == 0 && locks == 0
  UI-->>GC: ShutdownPhaseUiDone (4)
  GC->>TB: CANCEL_TIMERS (0x906) + XAML_SHUTDOWN (0x901)
  TB-->>GC: IsShutdownComplete ≤ 1.5 s
  GC->>EX: XAML_SHUTDOWN (0x801) + CHROME_REDRAW (0x803)
  EX-->>GC: Unadvise / Restore при активном S0
  GC->>IM: TrySetShutdownPhase(CoreDetaching = 1)
  GC->>CO: CallGlassCoreShutdown() + WM_GLASS_ANIM_DETACH (0xD00)
  CO->>CO: Hooks::Uninstall() · 33 Detours
  CO-->>GC: CoreDone (2)
  GC->>IM: TrySetShutdownPhase(WndDetaching = 5)
  GC->>WN: WM_GLASS_DETACH_* (0x202..0x224)
  WN->>WN: RemoveWindowSubclass на родном потоке
  WN-->>GC: WndDone (6) · g_hookInstalled = false
  GC->>GC: ScanStaleCallbacksBeforeFree (H91)
  GC->>CO: FreeLibrary (Core → Explorer → Taskbar → UI → Wnd)
  GC->>IM: ReportUnloadResult(pid, flags)
  GC->>GH: Unhook + HostShutdown (H71 ≤ 30 s)
  GH->>GH: H72 PostThreadMessage(WM_NULL) · inFlight == 0
  GH->>GH: FreeLibraryAndExitThread
```

## Что переносится, а что нет

| Блок сайта | В wiki | Комментарий |
|---|---|---|
| Архитектура 13 модулей | ✅ Mermaid `flowchart` + `subgraph` | Переключатели плоскостей → подграфы |
| `DoInitialize` FLOWCHART | ✅ Mermaid `flowchart TD` | Фильтры ветвей → `<details>` / `classDef` |
| IAT / PaintDispatcher PIPELINE | ✅ Mermaid `flowchart LR` | 4 канала — 4 ветки |
| GVT1 DATA FLOW | ✅ Mermaid `flowchart LR` | Полностью воспроизводимо |
| WindowClassifier CLASSIFICATION | ⚠️ частично | 8 шагов и 6 ролей — `flowchart`; интерактивный выбор роли — нет |
| LAYER STACK 3D (Explorer / Taskbar) | ❌ | Изометрии в Mermaid нет → статический SVG/PNG |
| UICompat COREWINDOW | ✅ Mermaid `flowchart` + `subgraph` | Граница AppContainer — `subgraph` |
| `ExecuteShutdown` SEQUENCE | ✅ Mermaid `sequenceDiagram` | 8 акторов читаемо, но шире, чем на сайте |
| SIMULATOR: IATRegistry, Taskbar corners | ❌ | Нужен JS → только сайт |
| Дерево реестра (ASCII-блок) | ✅ | Код-блок без изменений |

Ограничения GFM на GitHub: нет `<script>`, `<style>`, `iframe`, атрибутов
`style`/`onclick`; Mermaid рендерится с отключёнными кликами и не переопределяет
тему. Подробности публикации — в [README](README.md).
