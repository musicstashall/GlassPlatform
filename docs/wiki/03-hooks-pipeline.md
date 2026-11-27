# 03 · 33 модуля перехвата, IATRegistry и 4-канальный PaintDispatcher

`GlassCore.dll` содержит **33 файла `*Hooks.cpp`** (`BeginPaintHooks`, `DrawThemeHooks`,
`GdiTextHooks`, `MenuPaintHooks`, `SysColorHooks` и др.). Каждый перехват регистрируется
в `IATRegistry` без хардкода адресов, поддерживая безопасный 4-уровневый стек наложения
и диспетчеризацию `HDC`.

```mermaid
flowchart TB
  subgraph SRC["33 × *Hooks.cpp · user32 / gdi32 / uxtheme"]
    K1["BeginPaintHooks"]
    K2["DrawThemeHooks"]
    K3["GdiTextHooks"]
    K4["MenuPaintHooks"]
    K5["SysColorHooks"]
  end

  SRC --> IAT["IATRegistry<br/>без хардкода адресов"]
  IAT --> HD["HookDispatcher"]
  HD --> MAP["OverallHookMap + g_dcStack<br/>TLS Paint Context Stack"]
  MAP --> C1["CH 1 · CLIENT<br/>Begin/EndPaint · фон окна"]
  MAP --> C2["CH 2 · UXTHEME<br/>Theme Parts · темы и части"]
  MAP --> C3["CH 3 · NC FRAME<br/>NC / SysColor · рамки"]
  MAP --> C4["CH 4 · TEXT / GVT1<br/>Glow + Ink Capsule · 32bpp"]

  C1 --> FLY["FlyoutBorderPaintOrder<br/>01 BeginPaint → Zero-Alpha DIB<br/>02 FillRect → Backdrop Mask<br/>03 DrawTheme → Items + Hover<br/>04 DrawBorder · строго после заливки<br/>05 EndPaint → Commit to DWM"]

  classDef ch fill:#1f6feb1a,stroke:#1f6feb;
  class C1,C2,C3,C4 ch;
  classDef crit fill:#d299221a,stroke:#d29922;
  class FLY crit;
```

## Стек `IATRegistry` (4 слота)

На сайте — интерактивный симулятор; в вики — таблица эквивалентна по смыслу.

| Слот | Владелец | Поведение |
|---|---|---|
| 0 | Base · Original System API | Исходный `user32!BeginPaint`, вызывается последним |
| 0→1 | `GlassCore` | Активный владелец перехвата, рисует стекло |
| 1→2 | `WndCompat` | Сабклассинг контролов, поверх GlassCore |
| 2→3 | 3rd-Party | Внешний перехватчик; `Unhook Top` снимает только верхний слот |
| 3 | Свободен | Больше 4 слоёв не накладывается — защита от циклов |

Регистрация симметрична: `Push(GlassCore)` → `Push(WndCompat)` → `Push(3rd-Party)`,
снятие — строго с вершины (`Unhook Top`) либо полный `Reset` при выгрузке.

**Критический шаг.** `DrawBorder` в `FlyoutBorderPaintOrder` (шаг 04) обязан
выполняться **после** заливки фона: иначе DWM-композиция перезапишет рамку меню.

Источник: [docs.html · раздел 03](https://glass-platform.ru/docs.html) · ← [02](02-init) · → [04](04-gdi-32bpp)
