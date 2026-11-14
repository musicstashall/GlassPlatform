# Отрисовка и альфа (GDI)

**Связанный код:** `GlassShared/`, `GlassCore/src/Hooks/`

### Отрисовка и альфа: обычный GDI, без GDI+

Платформа **не использует GDI+** (`Gdiplus`) для корректной непрозрачности поверх «стекла». Зависимость от GDI+ в общих утилитах снята (см. комментарий в `GlassShared/include/BitmapUtils.h`). В **BWM** часто встречаются `GdipCreateBitmapAlpha` и другие вызовы GDI+ — при переносе в Glass их **не** воспроизводить; нужна эквивалентная схема на **GDI + DIB**.

**Опорные реализации в репозитории:**

- **`Shared::WinApiUtils::CreateSolidColorBrushWithAlpha`** — [`GlassShared/include/Helpers/WinApiUtils.h`](GlassShared/include/Helpers/WinApiUtils.h), [`GlassShared/src/WinApiUtils.cpp`](GlassShared/src/WinApiUtils.cpp): однопиксельный 32bpp DIB (premultiplied BGRA) и `CreatePatternBrush` — кисть с заданной альфой для заливок на прозрачном фоне.
- **`Shared::BitmapUtils`** — [`GlassShared/include/BitmapUtils.h`](GlassShared/include/BitmapUtils.h), [`GlassShared/src/BitmapUtils.cpp`](GlassShared/src/BitmapUtils.cpp): `Promise32BPP` (конвертация в 32bpp DIBSECTION с сохранением альфы), `HasAlphaChannel`, `SpriteEffect` / правка пикселей на месте для масок и «вырезания» фона.
- **Вывод с учётом альфа-канала битмапа** — типично `GdiAlphaBlend` (`msimg32.lib`, `BLENDFUNCTION`), см. поясняющие комментарии в [`GlassCore/src/Hooks/ListViewHooks.cpp`](GlassCore/src/Hooks/ListViewHooks.cpp) (там же зафиксировано отличие от BWM: после конвертации нельзя полагаться на обычный `BitBlt` для премультиплированной альфы).

При добавлении новых хуков ищи **существующий** паттерн в ближайшем `*Hooks.cpp` или в GlassShared, а не новый стек на GDI+.
