# rarext.dll: custom GDI на стекле (WinRAR zip properties)

График сжатия в свойствах `.zip` (shell extension `rarext.dll`) рисуется векторным GDI,
не bitmap/chroma. Настройки `Root\Control\{Kind}\ColorTreatAsTransparent*` к нему не применимы.

См. также [RootAdvancedIconBackground.md](RootAdvancedIconBackground.md) (bitmap/chroma для контролов).

## Детект

| Уровень | Условие |
|---------|---------|
| Холодный (`ClassifyRootWindow`) | `GetWindowModuleFileNameW` → `rarext.dll` → `PushPaint` IAT |
| Горячий (тела `Root_My*`) | `CallerBaseIs(caller, L"rarext.dll")` → иначе `orig` |

Проверка класса окна / `#32770` в hot path **не** используется.

## IAT-слоты на `rarext.dll`

| API | DLL | Слот |
|-----|-----|------|
| `GradientFill` | MSIMG32 (`GradientFill`) | `OHK_GRADIENTFILL` |
| `LineTo` | GDI32 | `OHK_LINETO` |
| `TextOutW` | GDI32 | `OHK_TEXTOUT` |

`SelectObject`, `PatBlt`, `ExtTextOutW` в import table rarext **нет** — не включаются.

Подмена кисти/пера — **внутри** draw-хуков (`LayeredStrokePen` в `LineTo`; вершины в `GradientFill`),
не через `OHK_SELECTOBJECT`.

## Поведение хуков

- **GradientFill:** sys-цвета вершин → `PatBlt(BLACKNESS)`; цветные столбцы → offscreen 32bpp + `GdiAlphaBlend`.
- **LineTo:** `GetCurrentObject(OBJ_PEN)` → `LayeredStrokePen` (как MMC wvc).
- **TextOutW:** `DrawTextWithAlpha` (+ TextGlow при spread Global).

## TextGlow

В `Root_OnBeginPaint` для `wndBase == rarext.dll`:
`TextGlow::RegisterTextGlowClient(hWnd, Global)` (с учётом `IsCategoryAllowedBySpreadMode`).

## Файлы

- `GlassCore/src/Hooks/RootWindowHooks.cpp` — классификация, glow
- `GlassCore/src/Hooks/RootGdiHooks.cpp` — тела хуков
- `GlassCore/Include/Hooks/RootGdiHooks.h` — `OHK_GRADIENTFILL`, `OHK_TEXTOUT`

## Тест

1. Свойства `.zip` в WinRAR: график + подпись «%» на стекле.
2. Glow Global — ореол у текста.
3. Регрессия COMDLG32 / MMC — caller ≠ rarext.
