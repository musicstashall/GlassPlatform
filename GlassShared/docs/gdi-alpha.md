# Отрисовка и альфа (GDI)

**Связанный код:** `GlassShared/`, `GlassCore/src/Hooks/`

### Отрисовка и альфа: обычный GDI, без GDI+

Платформа **не использует GDI+** (`Gdiplus`) для корректной непрозрачности поверх «стекла». Зависимость от GDI+ в общих утилитах снята (см. комментарий в `GlassShared/include/BitmapUtils.h`). В **BWM** часто встречаются `GdipCreateBitmapAlpha` и другие вызовы GDI+ — при переносе в Glass их **не** воспроизводить; нужна эквивалентная схема на **GDI + DIB**.

**Опорные реализации в репозитории:**

- **`Shared::WinApiUtils::CreateSolidColorBrushWithAlpha`** — [`GlassShared/include/Helpers/WinApiUtils.h`](GlassShared/include/Helpers/WinApiUtils.h), [`GlassShared/src/WinApiUtils.cpp`](GlassShared/src/WinApiUtils.cpp): однопиксельный 32bpp DIB (premultiplied BGRA) и `CreatePatternBrush` — кисть с заданной альфой для заливок на прозрачном фоне.
- **`Shared::BitmapUtils`** — [`GlassShared/include/BitmapUtils.h`](GlassShared/include/BitmapUtils.h), [`GlassShared/src/BitmapUtils.cpp`](GlassShared/src/BitmapUtils.cpp): `Promise32BPP` (конвертация в 32bpp DIBSECTION с сохранением альфы), `HasAlphaChannel`, `SpriteEffect` / правка пикселей на месте для масок и «вырезания» фона.
- **Вывод с учётом альфа-канала битмапа** — типично `GdiAlphaBlend` (`msimg32.lib`, `BLENDFUNCTION`), см. поясняющие комментарии в [`GlassCore/src/Hooks/ListViewHooks.cpp`](GlassCore/src/Hooks/ListViewHooks.cpp) (там же зафиксировано отличие от BWM: после конвертации нельзя полагаться на обычный `BitBlt` для премультиплированной альфы).

### Непрозрачный источник для native ROP — не color-key

`GlassCore::GdiUtils::OpaqueBitmapSource`
([header](../../GlassCore/Include/Subsystems/OpaqueBitmapSource.h),
[source](../../GlassCore/src/Subsystems/OpaqueBitmapSource.cpp)) используется
только проверенными VCL-путями `TToolBar!MaskBlt` (импорт в comctl32),
`TListView/TSystemListView!MaskBlt` (импорт фактического comctl32 paint-caller)
и EXE `StretchBlt` изображения `TPanel` внутри `TPanel`. Он временно снимает bitmap
с memory-DC для корректного `GetDIBits`, создаёт top-down 32bpp копию, требует
полный read и задаёт ей A=255 **без изменения RGB**, включая чёрные пиксели.
Затем исходный native API получает прежние source DC, mask/ROP/геометрию;
при выходе исходный bitmap выбран обратно, копия удалена. 1/4-bpp маски и
stock/OEM не преобразуются.

Это **отдельная политика opaque-копии**, а не замена preserve-alpha
`Promise32BPP`, не `ApplyBlackKeyAlpha` и не AlphaBlend произвольного strip.
Границы class/module/session/DC и исключение клиентского framebuffer задаёт
вызывающий hook. Нельзя применять этот helper к общему final blit или всему
окну ради DoubleBuffered. Подробности/ограничения —
[Delphi-VCL, тест 101](../../GlassCore/docs/Delphi-VCL.md#тест-101-непрозрачность-изображений-ttoolbar-и-вложенного-tpanel--native-bwm-пути)
и [тест 102](../../GlassCore/docs/Delphi-VCL.md#тест-102-tlistview--native-maskblt-паритет-restorator).

Ещё один потребитель — `GlassCore::RestoratorImages`: стартовые
картинки-подсказки Restorator в `TResTreeView` и `TImgViewBox`, SRCCOPY
`BitBlt`/`StretchBlt` из EXE. Helper не менялся и по-прежнему отдаёт A=255 с
неизменным RGB. Чёрную подложку удаляет сам модуль — только в полученной копии
и только если контур картинки чёрный; иначе копия отбрасывается и native читает
bitmap приложения. Альфа копии — попиксельная маска по яркости: A = max канала,
RGB без изменений, чёрное прозрачно, тёмное прозрачно настолько, насколько оно
тёмно. Копия с A=255 как есть там не выводится, поэтому источником может быть и
клиентский буфер. Это локальная политика одного модуля, не общий black-key:
[Delphi-VCL, тест 119](../../GlassCore/docs/Delphi-VCL.md#тест-119-restorator--стартовые-картинки-trestreeview-и-timgviewbox-без-чёрной-подложки)
(маршрут) и
[тест 120](../../GlassCore/docs/Delphi-VCL.md#тест-120-restorator--альфа-маска-по-яркости-вместо-блочной-политики)
(маска).

При добавлении новых хуков ищи **существующий** паттерн в ближайшем `*Hooks.cpp` или в GlassShared, а не новый стек на GDI+.
