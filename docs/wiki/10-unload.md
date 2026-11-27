# 10 · Детерминированная выгрузка `ExecuteShutdown()` (S0 + фазы 1–6)

Выгрузка инжектированных DLL во время работы UI-потоков — самый критичный участок
архитектуры. С сентября 2026 года `GlassCompat::ExecuteShutdown()` использует
event-based координацию (`PostMessage` + `WaitForSingleObject(INFINITE)`, без
блокирующего `SendMessage`) и строгий порядок демонтажа
**S0 → UICompat → TaskbarCompat → ExplorerCompat → GlassCore → WndCompat →
FreeLibrary + H71/H72 + FLAET**, фиксируя `ShutdownPhase (1..6)` и `UnloadResult`
в `InjectProcessMap`.

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
  Note over GC: S0 · Unloading · DiagLoader = false
  GC->>CO: GlassCore_BeginExplorerShellChromeRestore()
  GC->>IM: TrySetShutdownPhase(UiDetaching = 3)
  GC->>UI: WM_GLASS_UI_XAML_SHUTDOWN (0xC01)
  UI->>UI: COM drain obj == 0 + locks == 0
  UI-->>GC: ShutdownPhaseUiDone (4)
  GC->>TB: CANCEL_TIMERS (0x906) + XAML_SHUTDOWN (0x901)
  TB-->>GC: IsShutdownComplete, не более 1.5 s
  GC->>EX: XAML_SHUTDOWN (0x801) + CHROME_REDRAW (0x803)
  EX-->>GC: Unadvise / Restore при активном S0
  GC->>IM: TrySetShutdownPhase(CoreDetaching = 1)
  GC->>CO: CallGlassCoreShutdown() + WM_GLASS_ANIM_DETACH (0xD00)
  CO->>CO: Hooks::Uninstall(), 33 Detours
  CO-->>GC: CoreDone (2)
  GC->>IM: TrySetShutdownPhase(WndDetaching = 5)
  GC->>WN: WM_GLASS_DETACH_* (0x202..0x224)
  WN->>WN: RemoveWindowSubclass на родном потоке
  WN-->>GC: WndDone (6), g_hookInstalled = false
  GC->>GC: ScanStaleCallbacksBeforeFree (H91)
  GC->>CO: FreeLibrary Core → Explorer → Taskbar → UI → Wnd
  GC->>IM: ReportUnloadResult(pid, flags)
  GC->>GH: Unhook + HostShutdown (H71 ≤ 30 s)
  GH->>GH: H72 PostThreadMessage(WM_NULL), inFlight == 0
  GH->>GH: FreeLibraryAndExitThread, refcount 2 → 1 → 0
```

## Порядок фаз и почему он такой

| Фаза | Модуль | Барьер / защита |
|---|---|---|
| **S0** | `explorer.exe`: `BeginExplorerShellChromeRestore()` | `explorerframe` `ExtendFrame/DWM` → passthrough, пока XAML-поверхности активны |
| 1–4 | `UICompat` (3 → 4) | `WM_GLASS_UI_XAML_SHUTDOWN` на STA IXDE, COM drain `obj == 0 && locks == 0` (≤ 5 s), `IsComplete` ≤ 1.5 s |
| 2 | `TaskbarCompat` | `CANCEL_TIMERS` + `XAML_SHUTDOWN`, `RestoreAllFills`, `UnadviseWait`; строго **до** `ExplorerCompat` |
| 3 | `ExplorerCompat` | `XAML_SHUTDOWN` + `CHROME_REDRAW` на STA, до снятия `GlassCore` |
| 4 | `GlassCore` (1 → 2) | `CallGlassCoreShutdown()` под SEH H54, `WM_GLASS_ANIM_DETACH` + `Wait(∞)`, `Hooks::Uninstall()` 33 Detours |
| 5–6 | `WndCompat` (5 → 6) | `PostMessage(WM_GLASS_DETACH_*)` → `RemoveWindowSubclass` → `Wait(∞)` → `g_hookInstalled = false` |
| 6 | `FreeLibrary` + H71/H72 | H91 `ScanStaleCallbacksBeforeFree`, затем `FreeLibrary` (Core → Explorer → Taskbar → UI → Wnd); H71 `Wait` ≤ 30 s на ранний `HostShutdown`, H72 `PostThreadMessage(WM_NULL)` при `inFlight == 0` |

**Межмодульная зависимость.** `CHROME_REDRAW` в `ExplorerCompat` обязателен **после**
S0: иначе после выгрузки XAML окно Проводника останется с артефактом рамки.

На сайте фазы кликабельны (S0…6) с описанием действия и IPC — там же полный граф
возвратов.

Источник: [docs.html · раздел 10](https://glass-platform.ru/docs.html) · ← [09](09-registry) · → [11](11-tools)
