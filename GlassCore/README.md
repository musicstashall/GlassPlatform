# GlassCore

**Связанный код:** `GlassCore/`

### Ключевые подсистемы GlassCore

- **IATRegistry** — реестр IAT-слотов с single-occupancy стеком (first-owner-wins). Решает конфликт нескольких диспетчеров на одном слоте без цепочек и трамплинов.
  - В слоте всегда стоит **ровно один** detour — первый активный регистрант (вложенные сессии слот не флипают, паритет BWM `hookRef`); `outOriginal` — всегда настоящий оригинал. Стыковку сценариев между разными `*Hooks.cpp` делают **только** узкими телами хуков с passthrough в настоящий `orig`, **а не** экспортом вспомогательных API из одного модуля в другой (иначе теряется смысл разнесения по файлам и IATRegistry).
- **HookDispatcher\<N,M\>** — шаблонный диспетчер IAT-хуков для одного модуля. Делегирует все записи в IAT через IATRegistry. Не пишет в память напрямую.
- **OverallHookMap** — 3D-диспетчер: модуль → индекс хука → слот. Единая точка для всех per-module IAT-хуков.
- **PaintDispatcher** — глобальный Detours-перехватчик BeginPaint/EndPaint/GetDC/ReleaseDC/CreateCompatibleDC/DeleteDC. Четыре независимых списка провайдеров: `g_paintProviders` (pre-BeginPaint / EndPaint), `g_postPaintProviders` (после успешного BeginPaint — hdc и PAINTSTRUCT; при регистрации — UiRibbon для NetUI / NetUiToolWindow), `g_getDCProviders` (GetDC/ReleaseDC), `g_compatDCProviders` (CreateCompatibleDC/DeleteDC). Button/Static регистрируются **только** через `RegisterPaintProvider` — DeleteDC от uxtheme::EndBufferedPaint до них не доходит. Стек `g_dcStack` (thread_local) хранит пары (HWND, HDC) для поддержки вложенных сессий.
- **Animation** — анимации popup-окон (DComp + WAM). `~PopupIn()` использует `SendMessageTimeoutW` для синхронного снятия SubclassProc перед уничтожением объекта. **Порядок NC-рамки flyout:** [FlyoutBorderPaintOrder.md](docs/FlyoutBorderPaintOrder.md) — defer до uncloak, `FrameOnly` post-uncloak; не нарушать без явного запроса.

### GlassCore — на что обращать внимание

- Все IAT-записи идут через `IATRegistry`, никогда напрямую через `WriteMemory`
- `EnableHooksOnModule` / `DisableAllOverallHooks` — единственные точки включения/выключения хуков
- `g_overallMap` — единый 3D-диспетчер; `g_comboDispatcher` и `g_animDispatcher` удалены
- `g_overallHookDep` — порядок DLL в массиве имеет значение (форвардеры идут первыми для DrawTextW/DrawTextExW)
- Для каждой группы контролов использовать отдельную реализацию установки хуков в отдельных файлах
- `OHK_*` enum — при добавлении нового хука обновить `OHK_COUNT`
- **Несколько детуров на одном импорте (пример `BitBlt` в `comctl32`):** слот держит одного occupant'а — первый активный регистрант (стек в `IATRegistry`, вложенные сессии не флипают). Для сценария «полоса до иконок ListView» в `Engine::Initialize` сначала `ListViewControl::Startup()` (базовый ref на `BitBlt`), затем `ScrollBar::Startup()` — без перекрёстных вызовов между `*Hooks.cpp`.
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

## Документация

| Задача | Документ |
|--------|----------|
| IAT / PaintDispatcher | [docs/hooks-and-paint.md](docs/hooks-and-paint.md) |
| Перенос из BWM | [docs/bwm-and-donors.md](docs/bwm-and-donors.md) |
| Flyout NC / PopupIn | [docs/FlyoutBorderPaintOrder.md](docs/FlyoutBorderPaintOrder.md), [docs/DropDownAnimBorderMatrix.md](docs/DropDownAnimBorderMatrix.md) |
| Popup Menu/Tooltip Advanced | [docs/PopupAdvancedRendering.md](docs/PopupAdvancedRendering.md) |
| Скроллбар | [docs/ScrollbarRendering.md](docs/ScrollbarRendering.md) |
| Extension dialog GDI | [docs/ExtensionDialogGdi.md](docs/ExtensionDialogGdi.md) |
| Root advanced icon background | [docs/RootAdvancedIconBackground.md](docs/RootAdvancedIconBackground.md) |
| Текст / ink | [docs/glasscore-text.md](docs/glasscore-text.md) |
| GDI / альфа (GlassShared) | [GlassShared/docs/gdi-alpha.md](../GlassShared/docs/gdi-alpha.md) |
