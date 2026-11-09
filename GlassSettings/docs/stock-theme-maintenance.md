# Штатная тема перед удалением SecureUxTheme

## Рабочий путь и Native-помощник

WinUI переключает тему через `VisualStyleThemeManager.ApplyTheme` →
`ThemeManager2.TryAddAndSelectTheme`. Прямой `SetSystemVisualStyle` не эквивалентен
этому конвейеру: он не должен заменять применение полного `.theme` при подготовке
к удалению. Верный путь `.msstyles` сам по себе не доказывает применение цветов.

`Native/StockVisualStyle.h` реализует primary-путь Settings в нативном процессе:
1. Собственный STA-поток вне loader lock; `CoInitializeEx`/`CoUninitialize`.
2. Тот же CLSID/IID и порядок слотов `IThemeManager2`, что в C# ThemeManager2.
3. `Init(0)` → `AddAndSelectTheme(%WINDIR%\Resources\Themes\aero.theme)`:
   `IgnoreScreensaver` (32), `Silent` (4), тот же COM class context.
   **IgnoreColor не применяется**, системные цвета должны перейти со stock theme.
4. Только при `EnableSystemFont` вызывается ordinal 65 с flags=65 после успешного
   ThemeManager, как в Settings. Ordinal-only fallback намеренно не используется
   для удаления: он уже давал частичное применение/ошибки.
5. Успех всех вызовов + проверка точного текущего `aero.msstyles` под Windows.
   Ошибку API нельзя превратить в успех лишь по совпадению пути.

Применяется полная stock `.theme`, как в Settings, с её персонализацией (включая
обои/цвета/звуки), кроме screensaver. Обещание сохранения пользовательских обоев
не даётся. Возврат прежней custom theme при ошибке/rollback не выполняется.

## Контракт экспортов

Setup запускает `GlassSettings_SecureUxThemePrepareRemovalV2W` через доверенный
установленный Native **разрядности ОС**. WOW64 на x64 отказывается от операции.
Внешняя/legacy установка и ошибка query также не разрешают переключение/удаление.

V2 больше не является alias V1: при активном SUT применяет штатную тему через
описанный pipeline и возвращает 0 при успехе, фактический HRESULT при отказе.
Старый Setup уже принимает только нулевой результат; новый не маскирует ошибку
битовой комбинацией 0xC. V1 сохраняет прежний bitmask ABI, но использует тот же
новый apply helper для своего reset-path. Ни один экспорт не удаляет IFEO/COM
или файлы; после успешной подготовки Setup повторяет свои проверки профилей,
состояния и оригинального vendor MSI, только затем вызывает удаление.

Это изменение не исправляет общий путь применения пользовательских тем в
GlassSettings-x86 на x64. Необходима пересборка Native в составе Settings обеих
архитектур с обычным порядком сборки solution; отдельного нового экспорта нет.

Проверки Linux mocks исполняют фактические header/export и проверяют ABI-паритет
с C#, flags, STA, порядок, fonts, ошибки/cleanup/readback. Они не заменяют Windows
проверку цветов/scrollbars и реальное удаление. HRESULT логируется Setup;
дополнительные COM/font/readback поля DebugLog доступны в debug Native.
