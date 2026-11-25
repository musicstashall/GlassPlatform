# IAT, PaintDispatcher и узкие хуки

**Связанный код:** `GlassCore/src/Subsystems/`, `GlassCore/src/Hooks/`

### Ключевые подсистемы GlassCore

- **IATRegistry** — реестр IAT-слотов с single-occupancy стеком (first-owner-wins). Решает конфликт нескольких диспетчеров на одном слоте без цепочек и трамплинов.
  - В слоте всегда стоит **ровно один** detour — первый активный регистрант; остальные ждут в стеке без переустановки (паритет BWM `hookRef`, вложенные сессии не флипают). Освобождение возвращает слот следующему ждущему (или оригиналу). `outOriginal` — всегда настоящий оригинал слота, поэтому passthrough детуров всегда честный, а обрывов «хвост вызывает сырой API мимо соседей» нет как класса (раньше давали мерцание кастом/дефолт при churn'е сессий).
  - Вложенные сессии чужого сценария получают detour внешней сессии; узкие тела отдают чужие сценарии в настоящий `orig` — детерминировано. Стыковку сценариев между разными `*Hooks.cpp` делают **только** узкими телами хуков с passthrough в настоящий `orig`, **а не** экспортом вспомогательных API из одного модуля в другой (иначе теряется смысл разнесения по файлам и IATRegistry).
- **HookDispatcher\<N,M\>** — шаблонный диспетчер IAT-хуков для одного модуля. Делегирует все записи в IAT через IATRegistry. Не пишет в память напрямую.
- **OverallHookMap** — 3D-диспетчер: модуль → индекс хука → слот. Единая точка для всех per-module IAT-хуков.
- **PaintDispatcher** — глобальный Detours-перехватчик BeginPaint/EndPaint/GetDC/ReleaseDC/CreateCompatibleDC/DeleteDC. Четыре независимых списка провайдеров: `g_paintProviders` (pre-BeginPaint / EndPaint), `g_postPaintProviders` (после успешного BeginPaint — hdc и PAINTSTRUCT; при регистрации — UiRibbon для NetUI / NetUiToolWindow), `g_getDCProviders` (GetDC/ReleaseDC), `g_compatDCProviders` (CreateCompatibleDC/DeleteDC). Button/Static регистрируются **только** через `RegisterPaintProvider` — DeleteDC от uxtheme::EndBufferedPaint до них не доходит. Стек `g_dcStack` (thread_local) хранит пары (HWND, HDC) для поддержки вложенных сессий.
- **Animation** — анимации popup-окон (DComp + WAM). `~PopupIn()` использует `SendMessageTimeoutW` для синхронного снятия SubclassProc перед уничтожением объекта.

### GlassCore — на что обращать внимание

- Все IAT-записи идут через `IATRegistry`, никогда напрямую через `WriteMemory`
- `EnableHooksOnModule` / `DisableAllOverallHooks` — единственные точки включения/выключения хуков
- `g_overallMap` — единый 3D-диспетчер; `g_comboDispatcher` и `g_animDispatcher` удалены
- `g_overallHookDep` — порядок DLL в массиве имеет значение (форвардеры идут первыми для DrawTextW/DrawTextExW)
- Для каждой группы контролов использовать отдельную реализацию установки хуков в отдельных файлах
- `OHK_*` enum — при добавлении нового хука обновить `OHK_COUNT`
- **Несколько записей импорта с одним именем DLL.** `WalkIAT` и `WalkDelayloadIAT` просматривают все `IMAGE_IMPORT_DESCRIPTOR` / delay-дескрипторы с этим именем, а не только первый. Старый Delphi-линкер раскладывает один `user32.dll` на несколько записей (в Restorator.exe их три, и `DrawTextA` / `GetSysColor` / `FillRect` лежат не в первой). Возвращается по-прежнему один слот — первый найденный. Это не два загруженных модуля: два `comctl32` разделяет ModuleHandler по `HMODULE`.
- **Несколько детуров на одном импорте (пример `BitBlt` в `comctl32`):** слот держит одного occupant'а — первый активный регистрант (стек в `IATRegistry`, вложенные сессии не флипают); остальные ждут. Для сценария «полоса до иконок ListView» в `Engine::Initialize` сначала `ListViewControl::Startup()` (базовый ref на `BitBlt`), затем `ScrollBar::Startup()` — без перекрёстных вызовов между `*Hooks.cpp`.
- **ExplorerFrame `shlwapi` GDI-подмены** (`ExplorerFrameHooks.cpp`): `ExtTextOutW`/`SetBkColor` ставятся на IAT `shlwapi.dll` на весь срок жизни модуля, но чёрный фон применим только к DC корня File Explorer (`CabinetWClass`/`ExploreWClass` или уже отслеженный `g_trackedExplorerRoots`). DC оболочки (`Progman`/`WorkerW`, `Shell_TrayWnd`, их потомки) и memory-DC иконок идут в настоящий `orig` **без** `PatBlt(BLACKNESS)` — иначе иконки рабочего стола и панели получают чёрную маску (в логе: заливки `1920x48` и `16x16`). Вызов `SetBkColor` от чужого caller — passthrough с его исходным цветом; warning об отсутствии `orig` ограничен первыми 8 случаями, чтобы не засорять лог каждый paint.
- **UiRibbon** (`UiRibbonHooks.cpp`): IAT `UIRibbon.dll` в сессии `BeginPaint`/`EndPaint` для `NetUI` и `NetUiToolWindow`. Popup ленты: `NetUiToolWindow_Attach` на оболочке «Net UI Tool Window» / Layered (вызов с дочернего `NetUIHWND` в WinEvent, не отдельный CREATE-case) → `DropDownEventProvider::OnNetUiToolWindow*` → `GlassCore::UiRibbon`. Эти окна **не** `DropDown_Attach` (иначе `OnDropDownShow` делает их невидимыми). **GdiGradientFill** (по мотивам BWM `KbxLabelHooks`): только при `IsNetUiRibbonTooltipPaintSession()` — `PatBlt(BLACKNESS)` в клипе; иначе `orig`. Слот IAT может отсутствовать — смотри лог `GdiGradientFill=0`.
- **IeServer** (`IeServerHooks.cpp`): IAT только `StretchBlt` на `mshtml.dll`; сессия для окон с `GlassControlKind::InternetExplorer_Server` через `PaintDispatcher` (`RegisterPaintProvider` + `RegisterGetDCProvider`). Маркер HWND ставит `WndCompat::IeServer_Attach` (без субкласса). В хуке: копия кадра в 32bpp DIB, мягкая маска по «фоновым» цветам (белый, `COLOR_WINDOW`, серый MMC), опционально 3×3 по альфе, премультипликация и `GdiAlphaBlend` на целевой DC.
- **Animation**: `~PopupIn()` использует `SendMessageTimeoutW` (НЕ PostMessage!) для синхронного снятия SubclassProc. Нарушение — висячий dwRefData и краш после выгрузки DLL.
- **PaintDispatcher**: порядок регистрации — до `PaintDispatcher::Startup()`; порядок shutdown — `PaintDispatcher::Shutdown()` последним (после всех контролов). Каждая пара хуков работает со **своим** изолированным списком провайдеров:

  | Acquire              | Release              | Список провайдеров    | Регистрация                  |
  |----------------------|----------------------|-----------------------|------------------------------|
  | `HookBeginPaint`     | `HookEndPaint`       | `g_paintProviders`    | `RegisterPaintProvider`      |
  | `HookGetDC`          | `HookReleaseDC`      | `g_getDCProviders`    | `RegisterGetDCProvider`      |
  | `HookCreateCompatibleDC` | `HookDeleteDC`   | `g_compatDCProviders` | `RegisterCompatDCProvider`   |

  Дополнительно: внутри `HookBeginPaint` после успешного оригинального `BeginPaint` вызывается `g_postPaintProviders` (`RegisterPostPaintProvider`); коллбэк возвращает `bool` (фон очищен) — тогда диспетчер сбрасывает `PAINTSTRUCT::fErase`. Парного Release в диспетчере нет.

  Button и Static регистрируются **только** через `RegisterPaintProvider`. `HookDeleteDC` и `HookReleaseDC` обращаются исключительно к своим спискам и никогда не достигают `g_paintProviders` — это намеренно и не должно меняться.

### PopupOnly: привязка сессии к окну

В Full `g_activePaintHWnd` — вершина стека, и сессионные GDI-хуки действуют на весь поток. В PopupOnly этого мало: чужой `BeginPaint`/`GetDC` (TreeView, поле комбо, тулбар шапки диалога) шёл мимо стека и оставлял хостом уже закрытый попап, а memory-DC uxtheme кэширует без `DeleteDC`.

Запись стека несёт тип: `Paint`, `GetDC`, `Compat`, `Foreign`.

- **Foreign-барьер.** Неразрешённый kind при непустом стеке кладётся как `Foreign` без провайдеров, `GetPaintSessionHostHwnd()` на это время — `nullptr`. `IsForeignPaintBarrierActive()` — для сессионных хуков (ComboLBox `IsActive`). Пустой стек по-прежнему fast-path без записи.
- **CreateCompatibleDC.** Fallback на хост сессии только если `WindowFromDC(hdcSrc) == nullptr` и вершина не `Foreign`. Оконный DC чужого HWND к стеклянной сессии не приписывается.
- **Протухшие записи.** Перед Push/Pop в PopupOnly снимаются Compat-записи, у которых окно уничтожено, маркер kind снят или `GA_ROOT` скрыт после того, как был видим в момент push (снимок `AnimateWindow`/`WM_PRINT` ещё скрытого списка не срезается); на каждую — `onEnd` compat-провайдеров. `Foreign` с уничтоженным окном снимается без провайдеров. Закрытие `Paint`/`GetDC` так же снимает Compat/Foreign, созданные внутри этой сессии (повторный `DeleteDC` кэша запись уже не найдёт).
- Диагностика, не больше 32 раз на поток: `prune stale entry` и `foreign barrier`.
- **Attach-политика для popup'ов (PopupOnly).** Сессия открывается только на маркированные kind-окна. Матчинг attach-ветки в WinEventHandler по цепочке предков управляет, кто получает сессию: `Toolbar`/`TrackBar` (под `ViewControlClass`), теперь и `ListView`/`WinForms_ListView` (под `DropDown`/`ListviewPopup` — `WindowClassifier::IsUnderDropDownAncestor`, 8 уровней). Unmarked (`None`) — `Foreign`/fast-path без сессий, как раньше.

Сабкласс корня — один `RootSubclassProc` в любом режиме; режим проверяется на каждом сообщении, поэтому live-смена режима (`WinEventHook_SetMode`) не требует переустановки сабкласса. В PopupOnly он передаёт управление `PopupOnlyRootSubclassProc` (`WndCompat`): служебные `WM_GLASS_*`, меню-владелец, NC/активация и уничтожение уходят в общий обработчик как есть; `WM_COMMAND` (`CBN_DROPDOWN`) и `WM_CTLCOLORLISTBOX` приаттаченного ComboLBox (`BLACK_BRUSH`) обрабатываются там; остальное — `DefSubclassProc`. Узкий sink на чужом `#32770` — тот же прок с id 2. Class brush, SWCA и dark mode на чужой диалог не ставятся.
