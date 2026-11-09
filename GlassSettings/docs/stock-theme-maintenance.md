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
4. После выбора темы вызываются `GetCurrentTheme` и `SetCurrentTheme` с
   `applyNow=1`, теми же flags32/4: для обслуживания одного выбора записи
   недостаточно. Ошибка получения индекса/немедленного применения блокирует MSI.
5. Только при `EnableSystemFont` вызывается ordinal 65 с flags=65 после успешного
   ThemeManager, как в Settings. Ordinal-only fallback намеренно не используется
   для удаления: он уже давал частичное применение/ошибки.
6. Успех всех вызовов + проверка точного текущего `aero.msstyles` под Windows.
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


## Log18: успешный выбор не равен видимому переключению

Log18 показал success exit=0 и vendor MSI3010, но пользователь наблюдал прежнее
оформление. В старом success gate не было принудительного применения выбранной
записи; лог не доказывает видимое обновление окон и не содержит stage HRESULT.
Гипотеза: Aero уже записан как текущая тема (после прошлых попыток), и обычный
AddAndSelectTheme отработал как повторный выбор, а не новое применение.
Это не установленная по логу причина. Для исключения этого пути maintenance
теперь требует SetCurrentTheme(..., applyNow=1), даже при уже выбранном Aero.

После проверки точного пути до выхода короткоживущего rundll32 посылаются
WM_THEMECHANGED/WM_SYSCOLORCHANGE с SendMessageTimeout/SMTO_ABORTIFHUNG, чтобы
существующие окна инвалидировали кэши темы и системных цветов. Доставка в
каждое окно не является доказательством внешнего вида и не блокирует MSI из-за
чужого зависшего окна. Debug result дополнен forceHr. При отказе applyNow
возвращается фактический HRESULT и vendor MSI не запускается.

Отдельная перезагрузка нужна для выгрузки уже загруженного SUT (state4), но
она не заменяет штатное переключение стиля перед удалением. Mocks подтверждают
вызовы/порядок/отказы, не визуальное применение на реальной Windows.


## Доставка исправлений при одинаковой версии пакета

Изменённый Native должен попасть в **установленный** GlassSettings: vendor-only
removal не запускает DLL из исходной папки сборки Setup. Начиная с Log19 fix,
inspection сравнивает MsiFileHash установленного Native с проверенным предлагаемым
MSI и требует переустановку OS-native Settings при различии, даже при одинаковом
ProductVersion. В логе фиксируются compared/differs и оба MSI content hash.
Log19 без такой информации не доказывает, какая сборка helper выполнялась.


## Log20: недоступный hash и отсутствующая Release-диагностика

В запуске 20261109_145957 Settings64/32 installed/offered MSI=2.0.0.101,
но Native freshness compared=0/differs=0. Это unknown, не подтверждение совпадения.
План опять rows0x0/glassSettings0. В15:00:36.000 x64 helper exit0; vendor
MSI3010 в15:00:37.098, state4/restart1. Три missing HKU пропущены корректно.
Пользователь снова подтверждает отсутствие видимого переключения темы.

Исправлена ошибка предыдущего подхода: версионные PE-файлы обычно отсутствуют
в MsiFileHash; кроме того, существующий inner join может вернуть только прочие
неверсионные файлы MSI. Только этой таблицей нельзя обеспечивать свежесть Native.
Harvest теперь вызывает MsiGetFileHashW для **реального staged файла** с size=20
и сохраняет четыре DWORD в GLASSNATIVEMSIHASH внутри подписанного MSI. Для
Settings отсутствующая/неоднозначная Native или ошибка вычисления останавливает
сборку; старый табличный путь остаётся резервным для прежних пакетов. Плохое
свойство не заменяется старым hash и даёт unknown. При отсутствии метаданных
теперь есть явная строка причины, а не только compared=0. Политика явного
prerequisite, baseline integrity и ProductVersion не изменены.

Подробные HRESULT раньше шли через DebugLog, отключённый в Release. Добавлен
неавторизующий diagnostic mapping между Setup и OS-native helper: fixed-width
32-byte payload без указателей, локальное имя PID/tick, default same-user DACL,
отдельные результаты active/hr/COM/force/font/read/pathVerified. Setup пишет их
в обычный журнал после завершения процесса. Данные не заменяют exit code и
существующие проверки стиля/профилей. Нет записей на диск/в реестр. Старый Native
отклонит новый диагностический аргумент, и удаление остановится; требуется
установить пересобранный Native, а не обходить проверку. Отсутствующие результаты
явно логируются. pathVerified означает только совпавший путь, **не визуальный**
результат и не доказательство правильных цветов. Inactive no-op также различим.

Active query exit2 в log20 не подтверждает гипотезу inactive no-op; нет оснований
считать её причиной. Существующие profile preflight проверки сверяют desktop SID
с process token SID; запуск от другого UAC account также не установлен причиной.
Рендеринг/COM-алгоритм в этой итерации не менялся. Причина ложного визуального
успеха остаётся не установлена; новые stage results должны отделить реальное
выполнение COM/force от старой DLL/no-op и дать данные для следующего исправления.
Windows/MSVC/WiX-проверка недоступна; mock PASS не является visual acceptance.
Исходные log20 и vendor MSI log удалены после сохранения анализа.


### Log21: ANSI/Unicode dispatch в rundll32

Log21 (20261109_161115): Native source comparison теперь действительно сработал
(compared1/differs1 для обеих архитектур; installed MSI .101 → offered .103).
Явно выбраны обе GS: rows0x24; их MSI установились exit0 в16:12:41.030 и
16:12:47.985. Затем x64 style helper завершился **0x80070057 / E_INVALIDARG**
в16:12:48.469 без опубликованного trace. Vendor SUT MSI не запускался. Rollback
восстановил предыдущую установку (16:13:01.698). Missing HKU по-прежнему skipped.
Это не ошибка установки GS или vendor uninstall; новый diagnostic argument
выявил ошибку dispatch, которой предыдущие mock-тесты не моделировали.

Setup передавал rundll32 точное имя `GlassSettings_SecureUxThemePrepareRemovalV2W`.
Алгоритм rundll32 пробует requested+W как Unicode, requested+A и requested как
ANSI. Exact fallback не становится Unicode только оттого, что имя оканчивается
буквой W. Получался вызов LPWSTR callback с ANSI аргументом diagnostic mapping;
валидатор имени отклоняет такой буфер с E_INVALIDARG до применения темы. Раньше
пустой аргумент маскировал несовпадение сигнатуры. Совместимый алгоритм изучен в
https://github.com/wine-mirror/wine/blob/master/programs/rundll32/rundll32.c
(get_entry_point32); Windows machine-level trace здесь недоступен.

Исправлена только command line: requested base `...PrepareRemovalV2` без W.
Проверки PE/export/publisher/архитектуры остаются на реальном `...V2W`; LPWSTR
callback и строгая проверка имени mapping сохранены. Никаких новых версий
экспорта/искусственного обновления GS, bypass ошибки, fallback на x86 или
удаления hook вручную. Достаточно пересобрать Setup с этой правкой; существующие
пакеты с diagnostic Native должны быть установлены через явный prerequisite,
учитывая выполненный rollback. Модель lookup теперь проверяет actual production
команду и отдельно отвергает прежний exact-W ANSI dispatch.

Исправлен объяснимый новый отказ до применения темы. Это не подтверждение
устранения прежнего visible-style false success: после правильного вызова
ожидается строка stock-style stages с результатами COM/force/read, но видимое
переключение требует Windows acceptance. Исходные три журнала Log21 удалены
после фиксации анализа.
