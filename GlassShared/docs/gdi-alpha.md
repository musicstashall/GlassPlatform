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

Ещё два потребителя — `GlassCore::RestoratorImages` (стартовые подсказки и
графические ресурсы Restorator в `TResTreeView` и `TImgViewBox`, SRCCOPY
`BitBlt`/`StretchBlt` из EXE) и `TPanelDraw` для изображений графических
контролов в `TScrollBox` (SRCCOPY `StretchBlt` из EXE). Helper не менялся и
по-прежнему отдаёт A=255 с неизменным RGB; копия выводится как есть, вместе со
всем, на чём приложение собрало картинку. С готовой копией больше ничего не
делается: маска по яркости (тесты 119–120) и вырез цвета окна от контура
(тест 131) отменены. Картинка приходит уже сведённой, её сглаженный край —
смесь с фоном, и ни ключ, ни допуск прозрачность не восстановят.

Прозрачность такой картинки показывается иначе и не в этом helper:
`GlassCore::RestoratorCheckerboard` заменяет фон **до** сведения — заливку
цветом окна, которую EXE кладёт под картинку в memory-DC, — шахматным узором
(`CreateDIBPatternBrushPt`, без GDI+), и декодер приложения сам смешивает
картинку с ним. В окно при этом уходит та же непрозрачная копия.

[`PictureAlpha.h`](../../GlassCore/Include/Subsystems/PictureAlpha.h) теперь
только про значки: `DrawIconAlpha` — значок или курсор, который приложение
рисует `DrawIconEx`, рисуется оригиналом в собственные буферы на чёрном и на
белом, по разнице восстанавливается прозрачность каждого пикселя, и результат
один раз смешивается `GdiAlphaBlend`. Границы class/module/session/DC задаёт
вызывающий hook; подробности и пределы —
[Delphi-VCL, тест 131](../../GlassCore/docs/Delphi-VCL.md#тест-131-стили-ttoolbar-restorator--картинки-значки-и-подписи-tscrollbox)
(значки) и
[тест 132](../../GlassCore/docs/Delphi-VCL.md#тест-132-restorator--шахматная-подложка-вместо-выреза-цвета-окна)
(картинки и подложка).

При добавлении новых хуков ищи **существующий** паттерн в ближайшем `*Hooks.cpp` или в GlassShared, а не новый стек на GDI+.
