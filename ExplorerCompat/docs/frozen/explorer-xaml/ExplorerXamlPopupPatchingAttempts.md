# Патчинг XAML Popup меню Explorer — Документация

**Дата создания:** 2026-05-02  
**Последнее обновление:** 2026-05-15 — обнуление `SystemBackdrop` через `IVisualTreeService3::SetProperty(presenter, 0, idx)`; безымянные `Grid` не заливаются; `Visibility=Collapsed` на `*SystemBackdropRoot*` **всегда** (флаг реестра **`CollapseSystemBackdropRoot` снят**); blur: **`VTS CreateInstance(AcrylicBrush)`**, затем фабрика, затем solid; для **`MenuFlyoutPresenter`** при blur — **`DesktopAcrylicBackdrop`** на слот **`SystemBackdrop`** (в т.ч. после фикс. индекса **1736**).  
**Цель:** Изменить фон контекстных меню Explorer (WinUI 3) через XAML Diagnostics API

---

## Контекст

**Целевые окна:**
- Класс: `Microsoft.UI.Content.PopupWindowSiteBridge`
- Технология: WinUI 3 (не UWP)
- Процесс: `explorer.exe`
- Дефолтный backdrop: Системный эффект (Mica/Acrylic)

**Типы контекстных меню:**

### Меню A (Root Context Menu)
- **Описание:** Корневое контекстное меню (правый клик на рабочем столе, папке, файле)
- **XAML-структура:** Содержит элемент `SystemBackdropRoot` (Grid с именем, содержащим "SystemBackdropRoot")
- **Presenter:** `CommandBarFlyoutCommandBar` или `CommandBarOverflowPresenter`
- **Backdrop:** Рендерится **под** `SystemBackdropRoot`
- **Патчинг:** ✅ **РАБОТАЕТ** через `Visibility=Collapsed` на `SystemBackdropRoot`

### Меню B (Submenu)
- **Описание:** Подменю от корневого контекстного меню (наведение на пункт с подменю)
- **XAML-структура:** **НЕ содержит** элемент `SystemBackdropRoot`
- **Presenter:** `MenuFlyoutPresenter`
- **Backdrop:** Рендерится системой через свойство `SystemBackdrop` на `MenuFlyoutPresenter`
- **Патчинг:** ❌ **НЕ РАБОТАЕТ** — `SystemBackdropRoot` отсутствует, `SystemBackdrop` API недоступен через VTS

**Уточнение (2026-05-08):** кастомная кисть на подменю **стабильно** достигается в слое **GlassCore `WinUIBackdrop`** (Detours): трассировка **`Popup::put_SystemBackdrop`** / **`CPopup::SetSystemBackdrop`** обходит отложенную очередь **`ContentExternalBackdropLink`**, характерную для меню A. Подробнее — раздел «Наблюдения WinUIBackdrop + XAML Diagnostics (2026-05-08)».

**Инструменты:**
- XAML Diagnostics API (`IXamlDiagnostics`, `IVisualTreeService3`)
- Событие `AdviseVisualTreeChange` (Add/Remove элементов)
- Методы `GetProperty` / `SetProperty` / `CreateInstance`
- Консольная утилита **WinmdGuidDump** — извлечение IID WinRT-интерфейсов из `.winmd` (см. ниже)

## Диагностика видимого слоя (ExplorerCompat, 2026-05-15)

- В логе LogViewer искать **`[ExplorerCompat][Popup] Применена кисть`** — если таких строк нет при открытом меню, подмена `SolidColorBrush` через VTS не дошла до элемента.
- У **`MenuFlyoutPresenter`** вызывается **`SetProperty(handle, 0, systemBackdropIndex)`** (в т.ч. фиксированный индекс **1736**, если чтение `GetProperty` успешно). При **`E_*`** в логе — дефолтный системный слой может остаться; запасной путь: **`put_SystemBackdrop(nullptr)`** на объекте presenter в STA острова (QI к WinUI ABI) или узкий **Detours** на `put_SystemBackdrop` (**`GlassCore::WinUIBackdrop`**, см. раздел ниже про меню A/B).
- **`CollapseSystemBackdropRoot`**: **удалён** из политики (2026-05-15). `SystemBackdropRoot` скрывается **всегда** через `Visibility=Collapsed` в `XamlPopupPatcher`; старый DWORD в реестре игнорируется.
- Безымянный **`Microsoft.UI.Xaml.Shapes.Rectangle`**, дочерний к **`MenuFlyoutSeparator`**, дополнительно патчится как полоса между группами пунктов.
- Если визуально «как сток», временно задайте **контрастный** `LightMode_GradientColor` (например **`0xFFFF0000`**) и снова проверьте лог: должны появиться успешные **`SetProperty`** / **`Применена кисть`**.

---

## Утилита WinmdGuidDump (`tools/WinmdGuidDump`)

**Назначение:** из файла метаданных WinRT (`.winmd`) вывести **IID (GUID)** интерфейсов в виде фрагментов на C++ (`static const GUID ...`), чтобы в нативном коде вызывать `QueryInterface` к **Microsoft.UI.Composition** и другим API WinUI 3. Заголовки Windows SDK для UWP (`windows.ui.composition.h`) используют **другие** IID, чем `Microsoft.UI.Composition` в **Microsoft.UI.winmd**; без правильного GUID `QueryInterface` возвращает `E_NOINTERFACE`.

**Где лежит:** [tools/WinmdGuidDump](../tools/WinmdGuidDump) — отдельный проект .NET 8, не входит в `GlassProject.sln`.

**Сборка и запуск:**

```bat
cd /d <корень_репозитория_GlassProject>\tools\WinmdGuidDump
dotnet build -c Release
dotnet run -c Release -- "C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.vNext.CBS_8wekyb3d8bbwe\Microsoft.UI.winmd" Microsoft.UI.Composition
```

Аргументы:

1. **Путь к `.winmd`**. Если аргументов нет, в `Program.cs` задан путь по умолчанию к типичной папке Windows App Runtime на машине разработчика (его нужно проверить или заменить при другой установке). Иначе передайте полный путь к `Microsoft.UI.winmd` первым аргументом.
2. **Префикс пространства имён** (опционально, по умолчанию `Microsoft.UI.Composition`). Обрабатываются только **интерфейсы**, у которых `namespace` начинается с этой строки. В вывод попадают все такие интерфейсы с атрибутом `GuidAttribute`, а в stderr дублируется контрольный список «ожидаемых» имён (`ICompositionObject`, `ICompositor`, …) — если что-то помечено `MISSING`, значит в данном `.winmd` тип переименован, перенесён или файла недостаточно.

**Режим просмотра типов с «Composition» в имени** (классы и интерфейсы, любой namespace в файле):

```bat
dotnet run -c Release -- --list-composition "C:\путь\к\Microsoft.UI.winmd"
```

Полезно убедиться, что нужный контейнер метаданных — именно этот `.winmd` (часть типов WinUI живёт в `Microsoft.UI.winmd`, а не только в `Microsoft.UI.Xaml.winmd`).

**Формат вывода (основной режим):** строки вида `// Полное.имя.интерфейса` инициализатора `static const GUID IID_Microsoft_UI_Composition_<ShortName>`. Имена с вложенными пространства имён (например `Diagnostics.ICompositionDebugSettings`) получают короткий суффикс без префикса пространства — при переносе в код выровняйте имена констант под свой стиль или выберите строку из блока комментария с полным именем типа.

**Связь с репозиторием:** актуальные IID для `QueryInterface` к композиции в WinUI 3 вынесены в [GlassShared/include/MicrosoftUICompositionIIDs.h](../GlassShared/include/MicrosoftUICompositionIIDs.h). После обновления Windows App Runtime при сбоях `QueryInterface` пересоберите утилитой тот же набор интерфейсов и при необходимости обновите заголовок.

**Зависимости:** NuGet `System.Reflection.Metadata` (указан в `WinmdGuidDump.csproj`), .NET SDK 8+.

**Ограничения:** утилита читает только **объявленные в метаданных** WinRT-интерфейсы с `GuidAttribute`; внутренние COM-интерфейсы без winmd, а также типы из других merger-фрагментов, если они не в этом файле, нужно искать отдельно.

**Режим `--vtable` (раскладка виртуальной таблицы ABI):** для произвольного WinRT-интерфейса в том же `.winmd` выводятся номера слотов `IUnknown` (0–2), `IInspectable` (3–5 или три помеченные строки implicit), затем методы в порядке наследования цепочки интерфейсов (база → производный). Пример для проверки индексов при прямых вызовах по vtable (например `ICompositor::CreateColorBrush`, `ICompositionColorBrush::put_Color`):

```bat
dotnet run -c Release -- --vtable "C:\путь\к\Microsoft.UI.winmd" Microsoft.UI.Composition.ICompositor
dotnet run -c Release -- --vtable "C:\путь\к\Microsoft.UI.winmd" Microsoft.UI.Composition.ICompositionColorBrush
```

Колонки: слот, объявляющий тип, имя метода. **Важно:** индексы из winmd соответствуют публичному ABI типа; после обновления Windows App Runtime их стоит перепроверять. Для окончательной верификации на бинарнике можно сверить с `Microsoft.UI.Composition.dll` и PDB.

**Режим `--resolve-iids` (обратный поиск GUID → имя типа в `.winmd`):** перечислите один или несколько путей к файлам метаданных, затем GUID (с фигурными скобками или без). Утилита сканирует все `TypeDefinition` с `GuidAttribute` и печатает совпадения (интерфейс или тип). Полезно расшифровать список `IInspectable::GetIids` из натурного лога.

```bat
cd /d <корень_репозитория>\tools\WinmdGuidDump
dotnet run -c Release -- --resolve-iids "C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe\Microsoft.UI.winmd" "C:\Windows\SystemApps\Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe\Microsoft.UI.Xaml.winmd" "C:\Program Files (x86)\Windows Kits\10\UnionMetadata\10.0.26100.0\Windows.winmd" "{97F7A17B-97BE-5545-9F1C-0B9D44577F57}"
```

---

## MUX CompositionPropertySet: перечисление ключей и IID (2026-05-12)

### Вывод по логам (Windows App Runtime / MUX 3.x, acrylic меню Explorer)

На объекте `RuntimeClassName=Microsoft.UI.Composition.CompositionPropertySet`:

- **`QueryInterface`** к Foundation-коллекциям **`IIterable<IKeyValuePair<String,Object>>`** (IID `FE2F3D47-5D47-5499-8374-430C7CDA0204`) и **`IMapView<String,Object>`** (IID `BB78502A-F79D-54FA-92C9-90C5039FDF7E`) возвращает **`E_NOINTERFACE`**.
- В **`GetIids`** (в примере 14 элементов, без дублей интереса) **нет** перечисленных выше IID типов **`Windows.Foundation.Collections.*`**: объект **не заявляет** публичной проекции «словарь по `HSTRING` → читай через `IIterable/IMapView`».

Следствие для кода вида «обойти только реальные ключи из перечисления»: на этом классе упираемся не в качество клиентского перечислителя, а в **отсутствие опубликованных контрактов** для ключей через Foundation Collections на данном билде MUX.

### Таблица: IID из `GetIids` PropertySet → имя из `.winmd` (расшифровка `WinmdGuidDump --resolve-iids`)

Источники при прогоне: `Microsoft.UI.winmd` и `Microsoft.UI.Xaml.winmd` из  
`...\Microsoft.WindowsAppRuntime.CBS_8wekyb3d8bbwe\`, плюс `Windows.winmd` из UnionMetadata соответствующей версии SDK; для «дырок» дополнительно весь каталог `*.winmd` того же CBS (совпадений не найдено).

| IID | Имя в метаданных (если есть) |
|-----|-------------------------------|
| `{97F7A17B-97BE-5545-9F1C-0B9D44577F57}` | `Microsoft.UI.Composition.ICompositionPropertySet` |
| `{3DB6D621-3497-55E4-95BF-8AE5E7C34C33}` | `Microsoft.UI.Composition.ICompositionPropertySet2` |
| `{0E583D49-FB5E-5481-A426-D3C41E059A5A}` | `Microsoft.UI.Composition.ICompositionObject` |
| `{BCBBFEBF-799C-51CE-9C82-B6E49E7E62E1}` | `Microsoft.UI.Composition.ICompositionObject2` |
| `{D43C5CF1-47DF-52B8-B409-D5831503905E}` | `Microsoft.UI.Composition.ICompositionObject3` |
| `{029AD57C-5497-51F3-8B40-E4E235992C6F}` | `Microsoft.UI.Composition.ICompositionObject4` |
| `{85FD26D1-BB99-5392-9DCE-A05CB81458A5}` | `Microsoft.UI.Composition.ICompositionObject5` |
| `{8F56119D-B96D-58D0-9916-D1C5E390F890}` | `Microsoft.UI.Composition.IAnimationObject` |
| `{12B579A9-6A27-5CDE-A2A1-C557BB7DFDB3}` | `Microsoft.UI.Composition.Experimental.IExpCompositionPropertyChanged` |
| `{30D5A829-7FA4-4026-83BB-D75BAE4EA99E}` | `Windows.Foundation.IClosable` |
| `{46CAE093-BF9A-4B61-970F-8D9CFAFF944E}` | **не найден** в публичных `.winmd` CBS + `Windows.winmd` (вероятно внутренний COM/WRL уровня composition host) |
| `{83112A11-8E47-4B0C-A561-9E334D8F88E3}` | **не найден** (аналогично) |
| `{00000000-0000-0000-C000-000000000046}` | `IUnknown` (базовый COM) |
| `{AF86E2E0-B12D-4C6A-9C5A-D7AA65101E90}` | `IInspectable` (базовый WinRT) |

### План дальнейшей работы к цели «цвет и прозрачность» (без обязательного списка всех ключей)

1. **Критерий успеха** — устойчивое визуальное изменение меню; одного факта «`Insert*` / `TryGet*` вернул `Succeeded`» недостаточно (значение может лежать в PropertySet, но не участвовать в финальном графе кадра).
2. **Имена параметров** выводить из **документации эффекта / графа слоёв** и проверять **точечно** через существующие `TryGet*` и вложенные кисти (`GetSourceParameter`), а не ожидать полного ключ-листа через Collections API.
3. **Альтернативный слой**: усилить/добавить путь на уровне **подстановки кисти** (`put_CompositionBrush`) и уже существующих **DWM/DComp/SystemBackdrop** веток `WinUIBackdrop`, если мутации PropertySet оказываются «мёртвыми» для пикселя.
4. **Две нерасшифрованные IID** — следующий технический шаг только при необходимости: `Microsoft.UI.Composition.dll` с символами/IDA; в поставочных `.winmd` описания нет.

---

## Эксперименты с меню B (Submenu) — 2026-05-03

### Проблема

Меню B (подменю) **не содержит** элемент `SystemBackdropRoot`, поэтому рабочий метод `Visibility=Collapsed` не применим.

**Обнаруженная структура меню B:**
```
PopupWindowSiteBridge (HWND)
  └─ MenuFlyoutPresenter
      └─ [контент меню: ItemsControl, Button, Border, ...]
      └─ [НЕТ SystemBackdropRoot!]
```

### Эксперимент 1: Отложенная обработка MenuFlyoutPresenter

**Дата:** 2026-05-03 21:05  
**Гипотеза:** MenuFlyoutPresenter появляется **до** регистрации bridge HWND (`RegisterBridgeHwnd`), поэтому fallback-механизм не срабатывает  
**Подход:** Добавить очередь `g_deferredFlyoutPresentersForRootMark` (аналог `g_deferredDwxsForRootMark`) и обрабатывать отложенные MenuFlyoutPresenter в `FlushDeferredPopupRoots`

**Реализация:**
1. Добавлена глобальная очередь `std::vector<UINT64> g_deferredFlyoutPresentersForRootMark`
2. В fallback-секции (строки 1226-1231): если `bridgeCount == 0`, добавлять handle в очередь вместо пропуска
3. В `FlushDeferredPopupRoots`: после обработки DWXS обрабатывать отложенные FlyoutPresenter

**Результат:** ❌ **ПРОВАЛ**  
**Причина:** После маркировки MenuFlyoutPresenter как popup-корень, он становится `isInside = true`, и код пытается патчить его Background. Но специальная логика скрытия через `Visibility=Collapsed` (строки 1473-1486) скрывает **весь** MenuFlyoutPresenter, включая содержимое меню → меню становится невидимым.

**Лог:**
```
[ExplorerCompat][Popup] Fallback: маркирован Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter как popup-корень (bridgeCount=8): handle=515819720
[ExplorerCompat][Popup] MenuFlyoutPresenter обнаружен: type=Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter handle=515819720
```

**Откат:** Изменения откачены, код восстановлен из `.claude/backups/2026-05-03_1700/`

### Эксперимент 2: Диагностика свойств MenuFlyoutPresenter

**Дата:** 2026-05-03 21:14  
**Цель:** Выяснить, какие свойства доступны у MenuFlyoutPresenter через XAML Diagnostics API  
**Подход:** Использовать `DiagnoseElementProperties()` для вывода ключевых свойств

**Код:**
```cpp
else if (isMenuFlyoutPresenter)
{
    DebugLog(DBG_IMPORTANT,
        L"[ExplorerCompat][Popup] MenuFlyoutPresenter обнаружен: type=%ls handle=%llu",
        element.Type, static_cast<unsigned long long>(element.Handle));

    // Диагностика: вывести только ключевые свойства
    DiagnoseElementProperties(vts3, element.Handle, L"Background");
    DiagnoseElementProperties(vts3, element.Handle, L"Opacity");
    DiagnoseElementProperties(vts3, element.Handle, L"Visibility");
    DiagnoseElementProperties(vts3, element.Handle, L"SystemBackdrop");
}
```

**Результат:** ✅ **УСПЕХ** — обнаружено свойство `SystemBackdrop`

**Лог:**
```
21:14:33.992  [ExplorerCompat][Popup] MenuFlyoutPresenter обнаружен: type=Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter handle=456500392
21:14:33.992  [ExplorerCompat][XamlDiag] ═══ Диагностика свойств элемента (handle=0x1B35A4A8) ═══
21:14:34.003  [ExplorerCompat][XamlDiag]   Всего свойств: 319
21:14:34.005  [ExplorerCompat][XamlDiag]   Background = 572741352 (index=818 type=Microsoft.UI.Xaml.Media.Brush)
21:14:34.005  [ExplorerCompat][XamlDiag] ═══ Конец диагностики ═══
21:14:34.012  [ExplorerCompat][XamlDiag]   Всего свойств: 319
21:14:34.015  [ExplorerCompat][XamlDiag]   SystemBackdrop = 517309112 (index=1736 type=Microsoft.UI.Xaml.Media.SystemBackdrop)
```

**Ключевое открытие:**
- `Background` = 572741352 (handle кисти, не null)
- `Opacity` — не найдено (дефолтное значение)
- `Visibility` — не найдено (дефолтное значение = Visible)
- **`SystemBackdrop` = 517309112** (handle объекта backdrop, **не null**) ← **источник backdrop!**

### Эксперимент 3: Получение индекса SystemBackdrop через GetPropertyIndex

**Дата:** 2026-05-03 21:17  
**Гипотеза:** Свойство `SystemBackdrop` можно найти через `GetPropertyIndex` с полным именем класса  
**Подход:** Перебрать варианты имени свойства

**Результат:** ❌ **ПРОВАЛ** — все варианты возвращают `hr=0x80070057` (E_INVALIDARG)

**Лог:**
```
GetPropertyIndex(Microsoft.UI.Xaml.Controls.Control.SystemBackdrop) hr=0x80070057 idx=0
GetPropertyIndex(Microsoft.UI.Xaml.UIElement.SystemBackdrop) hr=0x80070057 idx=0
GetPropertyIndex(SystemBackdrop) hr=0x80070057 idx=0
```

**Вывод:** `GetPropertyIndex` не работает для свойства `SystemBackdrop`. Используем hardcoded index=1736 из диагностики.

### Эксперимент 4: Очистка SystemBackdrop через ClearProperty

**Дата:** 2026-05-03 21:34  
**Гипотеза:** `ClearProperty(index=1736)` очистит свойство `SystemBackdrop` и удалит backdrop  
**Подход:** Использовать hardcoded index=1736 из эксперимента 2

**Результат:** ❌ **ПРОВАЛ** — `ClearProperty` возвращает `S_OK`, но backdrop остаётся видимым

**Лог:**
```
GetProperty(SystemBackdrop index=1736) hr=0x00000000 backdrop=517309112
ClearProperty(index=1736) hr=0x00000000
[Backdrop присутствует]
```

**Вывод:** `ClearProperty` не удаляет backdrop, только сбрасывает значение в default (который тоже backdrop).

### Эксперимент 5: Удаление SystemBackdrop через SetProperty(null)

**Дата:** 2026-05-03 21:40  
**Гипотеза:** `SetProperty(element.Handle, 0, 1736)` установит `SystemBackdrop = null` и удалит backdrop  
**Подход:** Передать `0` (null) как значение свойства

**Результат:** ✅ **УСПЕХ** — backdrop полностью удалён, меню прозрачное!

**Лог:**
```
GetProperty(SystemBackdrop index=1736) hr=0x00000000 backdrop=517309112
SetProperty(MenuFlyoutPresenter.SystemBackdrop=null) hr=0x00000000
[Backdrop отсутствует, меню полностью прозрачное]
```

**Вывод:** `SetProperty` с `null` успешно удаляет backdrop. Это **рабочее решение** для меню B.

### Эксперимент 6: Создание кастомного backdrop через CreateInstance

**Дата:** 2026-05-04 07:54  
**Гипотеза:** После удаления backdrop можно создать свой через `CreateInstance` и установить через `SetProperty`  
**Подход:** Попробовать создать `MicaBackdrop`, `AcrylicBackdrop`, `DesktopAcrylicBackdrop`

**Результат:** ❌ **ПРОВАЛ** — все типы backdrop не создаются через `CreateInstance`

**Лог:**
```
CreateInstance(Microsoft.UI.Xaml.Media.MicaBackdrop) hr=0x80004003 handle=0
CreateInstance(Microsoft.UI.Xaml.Media.AcrylicBackdrop) hr=0x80004005 handle=0
CreateInstance(Microsoft.UI.Xaml.Media.DesktopAcrylicBackdrop) hr=0x80004003 handle=0
```

**Коды ошибок:**
- `0x80004003` = `E_POINTER` — недопустимый указатель
- `0x80004005` = `E_FAIL` — общая ошибка

**Вывод:** XAML Diagnostics API не может создавать backdrop объекты. Они создаются внутри WinUI 3 и недоступны через публичный API.

### Эксперимент 7: Доступ к backdrop через Composition API

**Дата:** 2026-05-04 08:06  
**Гипотеза:** Backdrop handle можно привести к `ICompositionObject` или `ICompositionBrush` для модификации  
**Подход:** `QueryInterface` на backdrop handle к интерфейсам Composition API

**Результат:** ❌ **ПРОВАЛ** — backdrop не поддерживает интерфейсы Composition API

**Лог:**
```
TryPatchBackdropViaComposition: backdropHandle=552179384
GetRuntimeClassName: Microsoft.UI.Xaml.Media.DesktopAcrylicBackdrop
QueryInterface(ICompositionObject) hr=0x80004002
QueryInterface(ICompositionBrush) hr=0x80004002
QueryInterface(ISystemBackdrop) hr=0x80004002
```

**Код ошибки:** `0x80004002` = `E_NOINTERFACE` — интерфейс не поддерживается

**Вывод:** `DesktopAcrylicBackdrop` — это XAML-обёртка, а не Composition API объект. Прямой доступ к композиции заблокирован.

### Эксперимент 8: Получение свойств backdrop через GetPropertyValuesChain

**Дата:** 2026-05-04 08:16  
**Гипотеза:** `GetPropertyValuesChain` вернёт собственные свойства backdrop (`TintColor`, `TintOpacity`, и т.д.)  
**Подход:** Вызвать `GetPropertyValuesChain` на backdrop handle

**Результат:** ❌ **ПРОВАЛ** — возвращает только attached properties, не собственные свойства backdrop

**Лог:**
```
GetPropertyValuesChain hr=0x00000000 propertyCount=108
[108 свойств: AutomationProperties.*, ToolTipService.*, Typography.*, ScrollViewer.*, ...]
```

**Вывод:** `GetPropertyValuesChain` возвращает только attached properties (прикреплённые извне), но не собственные свойства класса `DesktopAcrylicBackdrop`.

### Эксперимент 9: Получение индексов собственных свойств backdrop

**Дата:** 2026-05-04 08:19  
**Гипотеза:** `GetPropertyIndex` на backdrop handle вернёт индексы `TintColor`, `TintOpacity`, `FallbackColor`  
**Подход:** Вызвать `GetPropertyIndex` на backdrop handle с именами свойств

**Результат:** ❌ **ПРОВАЛ** — все свойства возвращают `E_INVALIDARG`

**Лог:**
```
GetPropertyIndex(TintColor) hr=0x80070057 index=0
GetPropertyIndex(TintOpacity) hr=0x80070057 index=0
GetPropertyIndex(TintLuminosityOpacity) hr=0x80070057 index=0
GetPropertyIndex(FallbackColor) hr=0x80070057 index=0
GetPropertyIndex(Kind) hr=0x80070057 index=0
```

**Вывод:** XAML Diagnostics API не предоставляет доступ к собственным свойствам `DesktopAcrylicBackdrop`. Объект — "чёрный ящик".

---

## Итоговые выводы по меню B (2026-05-04)

### ✅ Рабочее решение: SetProperty(SystemBackdrop=null)

**Метод:** `vts3->SetProperty(element.Handle, 0, 1736)` на `MenuFlyoutPresenter`

**Результат:**
- Backdrop полностью удалён
- Меню становится прозрачным (видны бордюры, текст, иконки)
- Фон патчится через обычные XAML-элементы (Border, Grid)

**Код (строки ~1480-1490 в XamlPopupPatcher.cpp):**
```cpp
if (element.Type && wcscmp(element.Type, L"Microsoft.UI.Xaml.Controls.MenuFlyoutPresenter") == 0)
{
    const unsigned systemBackdropIdx = 1736;
    InstanceHandle backdropHandle = 0;
    
    if (SUCCEEDED(vts3->GetProperty(element.Handle, systemBackdropIdx, &backdropHandle)) 
        && backdropHandle != 0)
    {
        HRESULT hrSetNull = vts3->SetProperty(element.Handle, 0, systemBackdropIdx);
        // hrSetNull должен быть 0x00000000 (S_OK)
    }
}
```

### ❌ Невозможно: Создание кастомного backdrop

**Причина:** XAML Diagnostics API не предоставляет доступ к Composition API

**Проверенные подходы:**
1. `CreateInstance(MicaBackdrop/AcrylicBackdrop/DesktopAcrylicBackdrop)` → E_POINTER / E_FAIL
2. `QueryInterface(ICompositionObject/ICompositionBrush)` → E_NOINTERFACE
3. `GetPropertyIndex(TintColor/TintOpacity/FallbackColor)` → E_INVALIDARG
4. `GetPropertyValuesChain` → возвращает только attached properties, не собственные свойства backdrop

**Архитектурное ограничение:**
- `DesktopAcrylicBackdrop` — это XAML-обёртка над внутренними Composition API объектами
- Composition API объекты создаются внутри WinUI 3 и недоступны через XAML Diagnostics
- Backdrop handle — это `IInspectable*`, но не `ICompositionObject*`
- Прямой доступ к композиции заблокирован на уровне WinRT

### Текущее состояние кода (2026-05-04 08:30)

**Файл:** `ExplorerCompat\xaml\Popup\XamlPopupPatcher.cpp`

**Что работает:**
- ✅ Удаление backdrop через `SetProperty(SystemBackdrop=null)` на `MenuFlyoutPresenter`
- ✅ Патчинг фона XAML-элементов меню B (Border, Grid, Button)
- ✅ Патчинг backdrop меню A через `Visibility=Collapsed` на `SystemBackdropRoot`

**Что добавлено для диагностики (можно удалить после финализации):**
- Функция `TryPatchBackdropViaComposition()` (строки ~104-220) — диагностика Composition API
- Определения интерфейсов `ICompositionObject`, `ICompositionBrush`, `ISystemBackdrop` (строки ~57-100)
- Логирование `GetPropertyValuesChain` и `GetPropertyIndex` для backdrop

**Рекомендация:** Очистить диагностический код после подтверждения, что `SetProperty(null)` — финальное решение.

---

## Архитектура XAML-дерева popup

### Меню A (Root Context Menu)

```
PopupWindowSiteBridge (HWND — полностью прозрачное окно)
  └─ CommandBarFlyoutCommandBar / CommandBarOverflowPresenter
      └─ Grid (ContentRoot / OuterContentRoot)
          └─ Grid (SystemBackdropRoot) ← КЛЮЧЕВОЙ ЭЛЕМЕНТ
              ├─ [backdrop-эффект рендерится ПОД этим контейнером]
              └─ [контент меню: ItemsControl, Button, Border, ...]
```

**Важно:**
- `SystemBackdropRoot` — это **контейнер-маркер**, а не сам backdrop
- Backdrop рендерится **под** XAML-деревом через внутренние механизмы WinUI 3
- Backdrop **не является** XAML-элементом или свойством, доступным через VTS
- **Патчинг работает:** `Visibility=Collapsed` на `SystemBackdropRoot` отключает backdrop

### Меню B (Submenu)

```
PopupWindowSiteBridge (HWND — полностью прозрачное окно)
  └─ MenuFlyoutPresenter [SystemBackdrop = 517309112 (handle объекта)]
      └─ [контент меню: ItemsControl, Button, Border, ...]
      └─ [НЕТ SystemBackdropRoot!]
```

**Важно:**
- `SystemBackdropRoot` **отсутствует** в XAML-дереве
- Backdrop управляется через свойство `SystemBackdrop` на `MenuFlyoutPresenter`
- Свойство `SystemBackdrop` **обнаружено** через `DiagnoseElementProperties` (index=1736, handle=517309112)
- **Патчинг в процессе:** попытка очистить `SystemBackdrop` через `ClearProperty`

---

## Рабочее решение: Visibility=Collapsed

**Статус:** ✅ **РАБОТАЕТ**  
**Файл:** `ExplorerCompat\xaml\Popup\XamlPopupPatcher.cpp`

### Как отключить backdrop

```cpp
// В OnVisualElementAdded при обнаружении SystemBackdropRoot
bool isSystemBackdropRoot = (element.Name && wcsstr(element.Name, L"SystemBackdropRoot"));

if (isSystemBackdropRoot)
{
    unsigned visibilityPropIdx = 0;
    if (SUCCEEDED(vts3->GetPropertyIndex(element.Handle,
        L"Microsoft.UI.Xaml.UIElement.Visibility", &visibilityPropIdx)))
    {
        BSTR enumType = SysAllocString(L"Microsoft.UI.Xaml.Visibility");
        BSTR enumValue = SysAllocString(L"Collapsed");
        InstanceHandle collapsedHandle = 0;
        
        if (SUCCEEDED(vts3->CreateInstance(enumType, enumValue, &collapsedHandle)))
        {
            HRESULT hrSet = vts3->SetProperty(element.Handle, collapsedHandle, visibilityPropIdx);
            // hrSet должен быть 0x00000000 (S_OK)
        }
        
        SysFreeString(enumType);
        SysFreeString(enumValue);
    }
}
```

### Почему это работает

1. `SystemBackdropRoot` — контейнер, который WinUI 3 использует как маркер для отрисовки backdrop
2. Скрытие контейнера через `Visibility=Collapsed` отключает весь backdrop-эффект
3. WinUI 3 проверяет видимость `SystemBackdropRoot` перед рендерингом backdrop
4. Используется стандартное XAML-свойство — не требуется доступ к внутренним API

---

## Свойства SystemBackdropRoot

**Обнаружено через `GetPropertyValuesChain` (2026-05-03):**

| Свойство | Тип | InstanceHandle (пример) | Назначение |
|----------|-----|-------------------------|------------|
| `Opacity` | `Windows.Foundation.Double` | `0x10B904A0` | Прозрачность элемента (0.0 - 1.0) |
| `OpacityTransition` | `Microsoft.UI.Xaml.ScalarTransition` | `0x0` (null) | Анимация изменения Opacity |
| `Visibility` | `Microsoft.UI.Xaml.Visibility` | `0xD76F570` | Видимость элемента (Visible/Collapsed) |
| `Background` | `Microsoft.UI.Xaml.Media.Brush` | `0x0` (null) | Фоновая кисть (не влияет на backdrop!) |
| `BackgroundTransition` | `Microsoft.UI.Xaml.BrushTransition` | `0x0` (null) | Анимация изменения Background |
| `BackgroundSizing` | `Microsoft.UI.Xaml.Controls.BackgroundSizing` | `0x10BA5920` | Масштабирование Background |

**Важно:** Числовые значения (например `0x10B904A0`) — это **InstanceHandle** (указатели на объекты в памяти WinUI), а не сами значения свойств.

### Экспериментальные заготовки

В коде `XamlPopupPatcher.cpp` (строки ~1060-1200) добавлены **закомментированные заготовки** для экспериментов:

0. **Visibility** — скрыть/показать backdrop (`Collapsed`/`Visible`) — **РАБОЧИЙ МЕТОД**
1. **OpacityTransition** — анимация прозрачности через `ScalarTransition`
2. **Background** — установка цветной кисти (**отключит blur!**)
3. **BackgroundTransition** — анимация фона через `BrushTransition`
4. **BackgroundSizing** — масштабирование фона (`InnerBorderEdge`/`OuterBorderEdge`)

Все заготовки содержат полный код создания объектов через `CreateInstance` и установки через `SetProperty`.

---

## Что НЕ работает (проверено экспериментально)

### ❌ Патчинг Background на SystemBackdropRoot

**Попытка:** Установить `SystemBackdropRoot.Background` в цвет из политики  
**Результат:** Backdrop **игнорирует** это свойство и продолжает отображаться в дефолтном состоянии  
**Причина:** Backdrop рендерится **не через `Background`**, а через внутренние механизмы WinUI 3

### ❌ Изменение Opacity на SystemBackdropRoot

**Попытка:** Установить `SystemBackdropRoot.Opacity` в значение из политики  
**Результат:** Backdrop **исчезает** (становится полупрозрачным без blur-эффекта)  
**Причина:** Любое изменение `Opacity` отключает системный backdrop-эффект WinUI 3

### ❌ Поиск свойства SystemBackdrop через VTS

**Попытка:** Найти свойство `SystemBackdrop` на презентерах (`FlyoutPresenter`, `MenuFlyoutPresenter`) или родителе `SystemBackdropRoot`  
**Результат:** Свойство **не найдено** через `GetPropertyIndex`  
**Причина:** `SystemBackdrop` API не экспортируется через XAML Diagnostics API

**Проверенные варианты имени:**
- `Microsoft.UI.Xaml.Controls.Control.SystemBackdrop`
- `Microsoft.UI.Xaml.UIElement.SystemBackdrop`
- `SystemBackdrop`

Все возвращают ошибку при `GetPropertyIndex`.

### ❌ DWM API (DwmSetWindowAttribute)

**Попытка:** Перехватить `DwmSetWindowAttribute` с атрибутом `DWMWA_SYSTEMBACKDROP_TYPE` (38) через Detours  
**Результат:** Вызовы **не обнаружены** для окон `PopupWindowSiteBridge`  
**Причина:** Backdrop применяется **не через DWM API**, а внутри XAML Islands

**Факты:**
- `DwmSetWindowAttribute` вызывается только для главного XAML острова (`XamlExplorerHostIslandWindow_WASDK`)
- Окна `PopupWindowSiteBridge` создаются **полностью прозрачными** без DWM backdrop

### ❌ DComposition API (CreateDevice / SetRoot)

**Попытка 1:** Перехватить `DCompositionCreateDevice` / `DCompositionCreateDevice2` через Detours  
**Результат:** Вызовы **не обнаружены** при открытии popup-меню  
**Причина:** DComp устройство создаётся **один раз** при инициализации и переиспользуется

**Попытка 2:** Создать `IDCompositionTarget` для popup HWND и заменить root visual  
**Результат:** Explorer **циклически крашится**  
**Причина:** Замена root visual **уничтожает XAML-контент** меню (кнопки, текст, иконки исчезают)

**Факты:**
- `CreateTargetForHwnd` успешно создаёт таргет
- `SetRoot` успешно устанавливает новый visual
- Но backdrop продолжает отображаться (он **под** root visual)
- XAML-контент исчезает → WinUI 3 крашится при обработке событий

---

## Где находится backdrop

**Backdrop НЕ находится в XAML-дереве.** Это визуальный эффект, который рендерится через:

1. **Composition API** — `IDCompositionVisual` с backdrop-эффектом (но не через публичный `CreateTargetForHwnd`)
2. **Внутренние механизмы WinUI 3** — `DesktopAcrylicController` / `MicaController` (недокументированные)
3. **НЕ через DWM** — `DwmSetWindowAttribute` не вызывается для popup

**Композиционная структура (предположительная):**
```
PopupWindowSiteBridge (HWND)
  └─ IDCompositionTarget (создан WinUI 3)
      ├─ Root Visual (XAML Islands контент) ← доступен через SetRoot
      │   └─ XAML-дерево (MenuFlyoutPresenter, SystemBackdropRoot, ...)
      └─ Backdrop Visual (НЕДОСТУПЕН через публичный API)
          └─ Системный backdrop-эффект (Mica/Acrylic)
```

---

## Текущее состояние кода (2026-05-03)

**Файл:** `ExplorerCompat\xaml\Popup\XamlPopupPatcher.cpp`

### Что реализовано

✅ Обнаружение `SystemBackdropRoot` через событие `AdviseVisualTreeChange`  
✅ Универсальная функция диагностики `DiagnoseElementProperties()` — работает для любого XAML-элемента  
✅ Функция `ExtractRealValue()` — извлекает реальные значения из InstanceHandle (Double, Int32/Enum, Boolean, String)  
✅ Декодирование enum-значений в строки (`Visibility`, `BackgroundSizing`, `ScrollBarVisibility`, `Orientation`, `HorizontalAlignment`, `VerticalAlignment`)  
✅ Заготовки для экспериментов с `Visibility`, `Opacity`, `Background`, `BackgroundSizing`, `OpacityTransition`, `BackgroundTransition`  
✅ Чтение политики из реестра (`BWM\Menu`: `GradientColor`, `BorderColor`, `EffectType`)

### Универсальная диагностика XAML-элементов

Функция `DiagnoseElementProperties()` автоматически перебирает **ВСЕ** свойства элемента через `GetPropertyValuesChain` и извлекает их реальные значения (не InstanceHandle).

```cpp
// Диагностика ВСЕХ свойств элемента (без фильтра)
DiagnoseElementProperties(vts3, elementHandle, L"ElementName", nullptr);

// Диагностика только свойств, содержащих "Opacity" в имени
DiagnoseElementProperties(vts3, elementHandle, L"ElementName", L"Opacity");

// Примеры вывода:
// [ExplorerCompat][Popup] ═══ Диагностика свойств: SystemBackdropRoot ═══
// [ExplorerCompat][Popup]   Всего свойств: 127
// [ExplorerCompat][Popup]   Opacity = 1.000000 (index=42 type=Windows.Foundation.Double hr=0x00000000)
// [ExplorerCompat][Popup]   OpacityTransition = 0x0 (index=43 type=Microsoft.UI.Xaml.ScalarTransition hr=0x00000000)
// [ExplorerCompat][Popup]   Visibility = 0 (Visible) (index=15 type=Microsoft.UI.Xaml.Visibility hr=0x00000000)
// [ExplorerCompat][Popup]   Background = 0x0 (index=89 type=Microsoft.UI.Xaml.Media.Brush hr=0x00000000)
// [ExplorerCompat][Popup]   BackgroundSizing = 0 (InnerBorderEdge) (index=90 type=Microsoft.UI.Xaml.Controls.BackgroundSizing hr=0x00000000)
// [ExplorerCompat][Popup] ═══ Конец диагностики ═══
```

**Преимущества:**
- Не нужно заранее знать, какие свойства есть у элемента
- Автоматически извлекает реальные значения для всех типов (Double, Int32/Enum, Boolean, String)
- Декодирует enum-значения в читаемые строки
- Можно фильтровать по части имени свойства или показать все

### Что закомментировано

🔧 Установка `Visibility=Collapsed` (заготовка 0) — **рабочий метод**, закомментирован для экспериментов  
🔧 Установка `Opacity` (заготовка 1) — отключает blur  
🔧 Все остальные заготовки (2-5) — для экспериментов

### Удалённый код (неверные направления)

- ❌ `CompositionHooks` (Detours на DWM/DComp API) — удалён 2026-05-02
- ❌ `DCompVisualInvestigation` (прямой доступ к визуалам) — удалён 2026-05-02
- ❌ `CustomBackdropRenderer` (замена root visual) — удалён 2026-05-02
- ❌ `g_pendingAcrylicLayers` / retry механизм — удалён 2026-05-02
- ❌ `ApplyAcrylicBrushProperty` (поиск AcrylicBrush) — удалён 2026-05-02
- ❌ `DisableSystemBackdrop` (поиск SystemBackdrop API) — удалён 2026-05-03
- ❌ `g_popupRootToPresenter` / `g_elementTypes` — удалены 2026-05-03
- ❌ Старый код диагностики с ручным извлечением Opacity — заменён на универсальную функцию 2026-05-03

---

## Ограничения XAML Diagnostics API

### Фундаментальные ограничения

1. **Backdrop не является XAML-свойством**
   - Не доступен через `GetProperty` / `SetProperty`
   - Не имеет типа XAML-элемента (`Rectangle`, `Border`, `Panel`)
   - Рендерится на уровне **ниже XAML** через Composition API

2. **SystemBackdrop API не экспортируется**
   - Свойство `SystemBackdrop` не найдено через `GetPropertyIndex`
   - Внутренние контроллеры (`DesktopAcrylicController`) недоступны
   - Публичные WinRT API не работают с popup-меню

3. **Изменение Background/Opacity отключает blur**
   - Установка `SystemBackdropRoot.Background` → backdrop игнорирует
   - Установка `SystemBackdropRoot.Opacity` → backdrop исчезает (становится полупрозрачным без blur)
   - WinUI 3 рендерит backdrop только когда эти свойства не изменены

### Что можно патчить

✅ **Видимость backdrop** — через `Visibility=Collapsed` на `SystemBackdropRoot`  
✅ **Фон XAML-элементов** — `Background` на контейнерах, кнопках, границах  
✅ **Прозрачность XAML-элементов** — `Opacity` на любых элементах (кроме `SystemBackdropRoot`)

❌ **Что нельзя патчить:**
- Цвет backdrop (blur-эффект)
- Альфа backdrop (прозрачность blur-эффекта)
- Тип backdrop (Mica → Acrylic или наоборот)

---

## Рекомендации

### НЕ пытаться

1. Искать `AcrylicBrush` через `GetProperty` — его нет в XAML-дереве
2. Использовать retry/deferred патчинг — не поможет, backdrop не устанавливается через XAML
3. Патчить `SystemBackdrop` через XAML Diagnostics — свойство не экспортируется
4. Применять DWM эффекты к `PopupWindowSiteBridge` — окно полностью прозрачное
5. Перехватывать `DwmSetWindowAttribute` / `DCompositionCreateDevice` — не вызываются для popup
6. Создавать `IDCompositionTarget` и заменять root visual — крашит explorer

### Возможные направления (требуют reverse engineering)

1. **Detours на внутренние функции WinUI 3**
   - `DesktopAcrylicController::ApplyBackdrop`
   - `MicaController::SetTintColor`
   - Высокий риск нестабильности, требует анализа `Microsoft.UI.Composition.dll`

2. **Поиск недокументированных COM-интерфейсов**
   - Дамп памяти WinUI 3 при открытом popup
   - Поиск vtable для backdrop-контроллеров
   - Reverse engineering внутренних структур

3. **Subclassing окна с кастомной отрисовкой**
   - Перехват `WM_PAINT` на `PopupWindowSiteBridge`
   - Отрисовка кастомного backdrop поверх прозрачного окна
   - Риск конфликта с XAML Islands

### Текущий компромисс

- Патчить то, что доступно (XAML-элементы: контейнеры, кнопки, границы)
- Отключать системный backdrop через `Visibility=Collapsed`
- Принять, что **изменение цвета/альфа backdrop невозможно** через XAML Diagnostics API
- Документировать ограничения для пользователя

---

## Эксперименты с Detours-хуками на Microsoft.UI.Xaml.dll (2026-05-04)

### Контекст

После провала всех попыток патчинга backdrop через XAML Diagnostics API было решено попробовать **низкоуровневый подход** — перехват функций внутри `Microsoft.UI.Xaml.dll` через библиотеку Detours.

### Инструменты

- **IDA Pro 9.3** — дизассемблер для анализа `Microsoft.UI.Xaml.dll`
- **PDB-символы** — загружены из Microsoft Symbol Server
- **Detours 4.0.1** — библиотека для runtime-хуков
- **GlassCore** — модуль платформы, подсистема Detours на `Microsoft.UI.Xaml.dll` в каталоге [`GlassCore/src/Hooks/xaml/`](../GlassCore/src/Hooks/xaml/) (см. раздел «Меню A vs меню B…» — карта файлов); публичный заголовок без изменений: [`GlassCore/Include/Hooks/WinUIBackdrop.h`](../GlassCore/Include/Hooks/WinUIBackdrop.h)

### Найденные функции через IDA Pro

| Функция | RVA | Сигнатура | Назначение |
|---------|-----|-----------|------------|
| `SystemBackdrop::OnTargetConnectedImpl` | `0x695fc4` | `HRESULT(__fastcall*)(void* pThis, void* compositionSupports, void* xamlRoot)` | Подключение backdrop к XAML-элементу |
| `MenuFlyoutPresenter::OnApplyTemplate` | `0x894890` | `HRESULT(__fastcall*)(void* pThis)` | Применение шаблона к popup меню |
| `XamlCompositionBrushBase::put_CompositionBrush` | `0x289290` | `HRESULT(__fastcall*)(void* pThis, void* brush)` | Установка composition brush для backdrop |
| `DCompTreeHost::SetSystemBackdropBrush` | `0x9507c0` | `HRESULT(__fastcall*)(void* pThis, void* brush)` | Установка backdrop на уровне DComp |

### Эксперимент 10: Перехват SystemBackdrop::OnTargetConnectedImpl

**Дата:** 2026-05-04 16:30  
**Гипотеза:** Функция вызывается при подключении backdrop к меню, можно логировать и изучать параметры  
**Подход:** Detours-хук на RVA `0x695fc4`

**Результат:** ✅ **УСПЕХ** — хук работает, функция вызывается дважды при открытии меню

**Лог:**
```
16:36:02.826  [GlassCore][WinUIBackdrop] Installing hooks for Microsoft.UI.Xaml.dll (0x00007FFAA6140000)
16:36:02.826  [GlassCore][WinUIBackdrop] Target addresses: OnTargetConnected=0x00007FFAA67D5FC4, put_CompositionBrush=0x00007FFAA63C9290
16:36:02.826  [GlassCore][WinUIBackdrop] DetourAttach(OnTargetConnected) result: 0
16:36:09.610  [GlassCore][WinUIBackdrop] Hooked_SystemBackdrop_OnTargetConnectedImpl CALLED! Process=Explorer.EXE
16:36:09.610  [GlassCore][WinUIBackdrop] Config: effectType=6, color=0x500000DD
16:36:09.611  [GlassCore][WinUIBackdrop] OnTargetConnectedImpl succeeded, backdrop connected
```

**Вывод:** Хук успешно перехватывает подключение backdrop, но не даёт возможности изменить его параметры.

### Эксперимент 11: Перехват MenuFlyoutPresenter::OnApplyTemplate

**Дата:** 2026-05-04 16:30  
**Гипотеза:** Функция вызывается при создании меню, можно получить доступ к XAML-элементу через `pThis`  
**Подход:** Detours-хук на RVA `0x894890`

**Результат:** ❌ **ПРОВАЛ** — RVA указывает на невалидную память, explorer.exe падает

**Лог:**
```
16:30:22.776  [GlassCore][WinUIBackdrop] Invalid OnApplyTemplate address: 0x00007FFB78B44890 (State=0x10000, Protect=0x1)
```

**Код ошибки:** `State=0x10000` = `MEM_FREE` — память не выделена

**Вывод:** RVA неверный или функция находится в другом месте. Требуется повторный анализ в IDA.

**Решение:** Хук временно отключён для предотвращения крашей.

### Эксперимент 12: Перехват XamlCompositionBrushBase::put_CompositionBrush

**Дата:** 2026-05-04 16:33  
**Гипотеза:** Функция вызывается при установке кисти backdrop, можно заменить кисть на кастомную  
**Подход:** Detours-хук на RVA `0x289290`

**Результат:** ⚠️ **ЧАСТИЧНО РАБОТАЕТ** — хук перехватывает установку кисти, но замена не работает

**Лог:**
```
16:36:09.721  [GlassCore][WinUIBackdrop] Hooked_XamlCompositionBrushBase_put_CompositionBrush CALLED!
16:36:09.721  [GlassCore][WinUIBackdrop] Brush runtime class: Microsoft.UI.Composition.CompositionEffectBrush
16:36:09.721  [GlassCore][WinUIBackdrop] Detected CompositionEffectBrush, creating replacement CompositionColorBrush
16:36:09.721  [GlassCore][WinUIBackdrop] Failed to create replacement brush, using original
```

**Обнаруженные типы кистей:**
1. `Microsoft.UI.Composition.CompositionColorBrush` — простая однотонная кисть
2. `Microsoft.UI.Composition.CompositionEffectBrush` — кисть с эффектом (Mica/Acrylic)

**Проблема:** Для меню используется `CompositionEffectBrush`, но создать замену `CompositionColorBrush` не удаётся.

### Попытка 12.1: QueryInterface для ICompositionObject

**Подход:** Получить `ICompositionObject` через `QueryInterface`, затем `Compositor` и создать новую кисть

**Код:**
```cpp
wil::com_ptr<ICompositionObject> compObj;
hr = pInspectable->QueryInterface(IID_PPV_ARGS(&compObj));
if (SUCCEEDED(hr)) {
    wil::com_ptr<ICompositor> compositor;
    hr = compObj->get_Compositor(&compositor);
    // ...
}
```

**Результат:** ❌ **ПРОВАЛ** — `QueryInterface` возвращает `E_NOINTERFACE` (`0x80004002`)

**Причина:** GUID из `Windows.UI.Composition` не совместимы с `Microsoft.UI.Composition`

### Попытка 12.2: Vtable-based вызов put_Color

**Подход:** Для `CompositionColorBrush` попробовать изменить цвет через прямой вызов метода по vtable

**Код:**
```cpp
void** vtable = *reinterpret_cast<void***>(pInspectable);
for (int idx = 20; idx <= 25; ++idx) {
    using PutColorFunc = HRESULT(__stdcall*)(void* pThis, Color color);
    PutColorFunc putColor = reinterpret_cast<PutColorFunc>(vtable[idx]);
    
    HRESULT tryResult = E_FAIL;
    __try {
        tryResult = putColor(pInspectable, color);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) {
        tryResult = E_FAIL;
    }
    
    if (SUCCEEDED(tryResult)) break;
}
```

**Результат:** ⚠️ **НЕ ПРОТЕСТИРОВАНО** — для меню приходит `CompositionEffectBrush`, а не `CompositionColorBrush`

**Вывод:** Метод может сработать для `CompositionColorBrush`, но для `CompositionEffectBrush` не применим.

### Попытка 12.3: Создание замены через Compositor (vtable-based)

**Подход:** Получить `Compositor` через vtable напрямую, без `QueryInterface`

**Статус:** 🔧 **В РАЗРАБОТКЕ** — код написан, но не протестирован

**Предполагаемый код:**
```cpp
// ICompositionObject::get_Compositor обычно по индексу 6-8
using GetCompositorFunc = HRESULT(__stdcall*)(void* pThis, void** compositor);
void** vtable = *reinterpret_cast<void***>(pInspectable);

for (int idx = 6; idx <= 8; ++idx) {
    GetCompositorFunc getCompositor = reinterpret_cast<GetCompositorFunc>(vtable[idx]);
    void* compositor = nullptr;
    
    __try {
        hr = getCompositor(pInspectable, &compositor);
        if (SUCCEEDED(hr) && compositor) {
            // Создать CompositionColorBrush через vtable
            // ...
        }
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { }
}
```

**Риски:**
- Индекс vtable может быть неверным → краш explorer.exe
- Требуется экспериментальный подбор индекса
- Даже при успешном получении `Compositor` создание кисти может не сработать

### Текущее состояние Detours-хуков (2026-05-04 16:42, код после split — 2026-05-07)

**Размещение:** каталог [`GlassCore\src\Hooks\xaml`](../GlassCore/src/Hooks/xaml/) — установка/trampoline и точки входа в первую очередь в `WinUIBackdropHooks.cpp` и `WinUIBackdropAttach.cpp`; общее состояние/timeline и учёт нескольких `ContentExternalBackdropLink` — `WinUIBackdropState.cpp`.

**Что работает:**
- ✅ Хук `SystemBackdrop::OnTargetConnectedImpl` — логирует подключение backdrop
- ✅ Хук `XamlCompositionBrushBase::put_CompositionBrush` — определяет тип кисти
- ✅ Чтение конфигурации из реестра (`HKCU\Software\StanislavKa\BlackWindowManager\Menu`)

**Что не работает:**
- ❌ Хук `MenuFlyoutPresenter::OnApplyTemplate` — невалидный RVA, отключён
- ❌ Замена `CompositionEffectBrush` на `CompositionColorBrush` — не удаётся создать новую кисть
- ❌ Изменение цвета backdrop — backdrop остаётся дефолтным (Mica)

**Что в разработке:**
- 🔧 Vtable-based доступ к `Compositor` для создания кисти

### Архитектурная проблема

**Основная проблема:** `Microsoft.UI.Composition` использует **другие GUID** для COM-интерфейсов, отличные от `Windows.UI.Composition`.

**Факты:**
1. `QueryInterface` с GUID из `Windows.UI.Composition` возвращает `E_NOINTERFACE`
2. Стандартные WinRT-интерфейсы (`ICompositionObject`, `ICompositor`, `ICompositionBrush`) не работают
3. `Microsoft.UI.Composition` — это проекция WinRT, но с изменёнными GUID
4. Публичные заголовки (`windows.ui.composition.h`) содержат GUID для UWP, а не для WinUI 3 desktop

**Возможные решения:**
1. **Vtable-based доступ** — обходить `QueryInterface`, вызывать методы напрямую по vtable
2. **Reverse engineering GUID** — найти правильные GUID для `Microsoft.UI.Composition` через дамп памяти
3. **Detours на CreateColorBrush** — перехватить создание кисти на уровне `Compositor`

### Сравнение подходов: XAML Diagnostics vs Detours

| Аспект | XAML Diagnostics API | Detours на Microsoft.UI.Xaml.dll |
|--------|----------------------|----------------------------------|
| **Доступ к XAML-дереву** | ✅ Полный | ❌ Нет |
| **Доступ к Composition API** | ❌ Нет | ⚠️ Частичный (через vtable) |
| **Удаление backdrop** | ✅ `SetProperty(SystemBackdrop=null)` | ❌ Не реализовано |
| **Создание кастомного backdrop** | ❌ Невозможно | ⚠️ В разработке (vtable-based) |
| **Стабильность** | ✅ Высокая | ⚠️ Средняя (риск крашей) |
| **Зависимость от версии DLL** | ✅ Нет | ❌ Да (RVA могут измениться) |

### Рекомендации

**Для удаления backdrop:**
- Использовать **XAML Diagnostics API** (`SetProperty(SystemBackdrop=null)`) — стабильно и работает

**Для создания кастомного backdrop:**
- Попробовать **vtable-based доступ** к `Compositor` — высокий риск, но единственный путь
- Если не сработает — принять ограничение и документировать

**НЕ пытаться:**
- Считать IID `Microsoft.UI.Composition` равными IID из `windows.ui.composition.h` (UWP) — для WinUI 3 desktop они **разные**; актуальные значения — из `Microsoft.UI.winmd` (утилита **WinmdGuidDump**) или [MicrosoftUICompositionIIDs.h](../GlassShared/include/MicrosoftUICompositionIIDs.h)
- Использовать `MenuFlyoutPresenter::OnApplyTemplate` — RVA неверный, требует повторного анализа

---

## История изменений

| Дата | Изменение |
|------|-----------|
| 2026-05-02 | Создан документ |
| 2026-05-02 | Удалён retry механизм и поиск AcrylicBrush |
| 2026-05-02 | Проведён эксперимент с API Hooking (Detours) — провалился, код удалён |
| 2026-05-02 | Проведён эксперимент с DComposition Visual Investigation — провалился, код удалён |
| 2026-05-02 | Проведён эксперимент с Custom Backdrop через IDCompositionDevice — провалился, код удалён |
| 2026-05-03 | Исправлена проблема накопления bridge HWND из bootstrap scan |
| 2026-05-03 | Восстановлен рабочий подход `Visibility=Collapsed` на `SystemBackdropRoot` |
| 2026-05-03 | Добавлены экспериментальные заготовки для свойств `SystemBackdropRoot` |
| 2026-05-03 | Документ переписан: удалена чушь, оставлены только проверенные факты |
| 2026-05-03 | Добавлены универсальные функции диагностики `DiagnoseElementProperties()` и `ExtractRealValue()` |
| 2026-05-03 | Реализовано извлечение реальных значений из InstanceHandle (Double, Int32/Enum, Boolean, String) |
| 2026-05-03 | Добавлено декодирование enum-значений в строки (Visibility, BackgroundSizing, ScrollBarVisibility, Orientation, Alignment) |
| 2026-05-03 21:21 | Добавлены определения типов меню A (Root Context Menu) и B (Submenu) |
| 2026-05-03 21:21 | Зафиксированы эксперименты с меню B: отложенная обработка (провал), диагностика свойств (успех), очистка SystemBackdrop (в процессе) |
| 2026-05-03 21:21 | Обнаружено свойство `SystemBackdrop` на `MenuFlyoutPresenter` (index=1736, handle=517309112) |
| 2026-05-04 08:30 | Завершены эксперименты 3-9 с меню B: все попытки создания кастомного backdrop провалились |
| 2026-05-04 08:30 | Подтверждено рабочее решение: `SetProperty(SystemBackdrop=null)` успешно удаляет backdrop |
| 2026-05-04 08:30 | Документированы архитектурные ограничения XAML Diagnostics API для доступа к Composition API |
| 2026-05-04 08:30 | Добавлены итоговые выводы: удаление backdrop возможно, создание кастомного — невозможно |
| 2026-05-04 16:43 | Добавлен раздел "Эксперименты с Detours-хуками на Microsoft.UI.Xaml.dll" |
| 2026-05-04 16:43 | Документированы эксперименты 10-12: перехват функций через IDA Pro + Detours |
| 2026-05-04 16:43 | Обнаружена проблема: `Microsoft.UI.Composition` использует другие GUID, отличные от `Windows.UI.Composition` |
| 2026-05-04 16:43 | Хук `put_CompositionBrush` работает, но замена кисти не удаётся через `QueryInterface` |
| 2026-05-04 16:43 | Предложен vtable-based подход для обхода `QueryInterface` — в разработке |
| 2026-05-04 16:43 | Добавлена таблица сравнения подходов: XAML Diagnostics vs Detours |
| 2026-05-04 | Утилита **WinmdGuidDump** вынесена в `tools/WinmdGuidDump`; в документ добавлен раздел с инструкцией; рекомендация по IID синхронизирована с winmd |
| 2026-05-07 | Рефакторинг GlassCore: `WinUIBackdrop` разнесён по `GlassCore/src/Hooks/xaml/*.cpp`; общие типы/extern — `GlassCore/Include/Hooks/xaml/WinUIBackdropInternal.h`. Документ: актуализированы пути; добавлены результаты smoke-тестов меню A (1-е/2-е открытие). |
| 2026-05-08 | Раздел «Наблюдения WinUIBackdrop + XAML Diagnostics»: два MUX в explorer, меню A первое/второе (ColorBrush→EffectBrush vs только EffectBrush, defer-pre/passthrough), логи `[deferQ]`/`[deferSnap]`, сравнение канала A (**ContentExternalBackdropLink**) vs B (**Popup** / `put_SystemBackdrop`), сценарий A→B→A→B, уточнена таблица тестов меню A (регрессия второго открытия). |
| 2026-05-08 | Подраздел **«Матрица: что пробовали для второго открытия меню A»**: зафиксированы отброшенные ветки (replacement-only, heavy null-path, MUX+replacement+link, и т.д.); в коде **`WinUIBackdropHooks`** для повторного показа при липком флаге — **только `CallOriginal(MUX)`** (стабильность). |

### Эксперимент 13: Блокировка OnTargetConnectedImpl с кэшированием

**Дата:** 2026-05-04 18:47  
**Гипотеза:** Возврат `E_NOTIMPL` из `OnTargetConnectedImpl` удаляет backdrop, но повторные вызовы для того же `pThis` вызывают крэш explorer.exe при наведении на соседние пункты меню с открытым субменю  
**Подход:** Кэшировать `pThis` объектов, для которых уже вернули `E_NOTIMPL`, и при повторных вызовах возвращать `S_OK` (fake success) без вызова оригинала

**Реализация:**
```cpp
// Кэш заблокированных backdrop объектов (pThis)
static std::unordered_set<void*> g_blockedBackdrops;
static std::mutex g_blockedBackdropsMutex;

static HRESULT __fastcall Hooked_SystemBackdrop_OnTargetConnectedImpl(
    void* pThis, void* compositionSupports, void* xamlRoot)
{
    // Проверяем, был ли этот backdrop уже заблокирован
    {
        std::lock_guard lock(g_blockedBackdropsMutex);
        if (g_blockedBackdrops.find(pThis) != g_blockedBackdrops.end())
        {
            DebugLog(DBG_IMPORTANT,
                L"[GlassCore][WinUIBackdrop] pThis=0x%p already blocked, returning S_OK (fake success)",
                pThis);
            return S_OK;
        }
    }
    
    // Читаем конфигурацию из реестра
    DWORD effectType = 0;
    COLORREF color = 0;
    if (!ReadMenuConfig(effectType, color))
    {
        return g_original_OnTargetConnectedImpl(pThis, compositionSupports, xamlRoot);
    }
    
    // Добавляем в кэш заблокированных
    {
        std::lock_guard lock(g_blockedBackdropsMutex);
        g_blockedBackdrops.insert(pThis);
    }
    
    // Блокируем подключение backdrop — возвращаем ошибку только первый раз
    DebugLog(DBG_CRITICAL,
        L"[GlassCore][WinUIBackdrop] *** BLOCKING OnTargetConnectedImpl (first time) - returning E_NOTIMPL ***");
    return E_NOTIMPL;
}
```

**Результат:** ⚠️ **НЕ ПРОТЕСТИРОВАНО** — сборка не завершена (LNK1168: GlassCompat.dll заблокирован запущенной платформой)

**Ожидаемое поведение:**
- ✅ Backdrop должен быть удалён (как подтверждено пользователем ранее)
- ✅ Explorer НЕ должен крашиться при наведении на соседние пункты меню с открытым субменю
- ✅ Логи должны показывать "pThis=0x%p already blocked, returning S_OK (fake success)" для повторных вызовов

**Проблема, которую решает:**
- При открытии субменю WinUI вызывает `OnTargetConnectedImpl` **несколько раз** для одного и того же backdrop объекта
- Первый вызов: возвращаем `E_NOTIMPL` → backdrop не подключается
- Последующие вызовы (при наведении на соседние пункты): возвращаем `S_OK` → WinUI думает, что backdrop подключён, не пытается повторно и не крашится

**Статус:** 🔧 **ТРЕБУЕТ ТЕСТИРОВАНИЯ** — необходимо остановить платформу, собрать проект и проверить поведение

**Альтернативный подход (если эксперимент провалится):**
- Вернуться к **XAML Diagnostics API** (эксперимент 5): `SetProperty(SystemBackdrop=null)` на `MenuFlyoutPresenter`
- Это **стабильное решение**, которое гарантированно работает без крэшей

---

| 2026-05-04 18:47 | Добавлен эксперимент 13: блокировка OnTargetConnectedImpl с кэшированием (не протестирован) |

---

## Меню A vs меню B, слой SystemBackdropRoot и Detours (GlassCore, 2026-05-07)

**Контекст:** у корневого контекстного меню (меню A) в дереве есть Grid **SystemBackdropRoot** — под ним рисуется системный backdrop. Через XAML Diagnostics подтверждено: скрытие этого узла (`Visibility=Collapsed`) убирает видимый backdrop. Подмена кисти для кастомного тинта должна попадать в **ту же композиционную цепочку**, что и этот слой, а не только в произвольный `CompositionEffectBrush` на пути шаблона.

**Разница трасс в логах (факт):**

| Сценарий | Типичные хуки | Кисть (runtime class) |
|---------|----------------|------------------------|
| Меню A | `OnTargetConnectedImpl` → (раньше без Compositor) → `XamlCompositionBrushBase::put_CompositionBrush` на узле вроде **AcrylicBrush**; плюс `DCompTreeHost::SetSystemBackdropBrush` по необходимости | `Microsoft.UI.Composition.CompositionEffectBrush` (MUX/CBS) |
| Меню B | `Popup::put_SystemBackdrop` / `CPopup::SetSystemBackdrop` | Часто `Windows.UI.Composition.CompositionEffectBrush` |

**Карта реализации в коде (после рефакторинга — несколько TU в `Hooks/xaml/`):**

| Файл | Назначение |
|------|------------|
| [`WinUIBackdropInternal.h`](../GlassCore/Include/Hooks/xaml/WinUIBackdropInternal.h) | Общие объявления, extern глобалов между TU |
| [`WinUIBackdropHooks.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropHooks.cpp) | Обёртки Detours (`Hooked_*`), `Initialize`/`Shutdown`, лог точек входа, вызов `TryReapplyBackdropViaCompositionSupports` после оригинала |
| [`WinUIBackdropState.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropState.cpp) | TLS/depth, несколько активных `ContentExternalBackdropLink` (ref-count), **`LogWinUiBackdropTimeline`** |
| [`WinUIBackdropReapply.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropReapply.cpp) | **`TryReapplyBackdropViaCompositionSupports`**, **`TryResolveCompositorViaIxamlRootGetContent`**, отложенный `put_SystemBackdrop`, ветка «повторное меню A», лог строк про Compositor/link |
| [`WinUIBackdropComposition.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropComposition.cpp) | Композиция MUX / vtable-доступ к compositor там, где применимо |
| [`WinUIBackdropEffectBrush.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropEffectBrush.cpp) | Мутация/обход без «ломания» меню при null-path на link |
| [`WinUIBackdropAttach.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropAttach.cpp) | `DetourAttach`/`CallOriginal`, тяжёлый путь **`put_CompositionBrush`** (priming, deferred pre по link), `FlyoutBase::put_SystemBackdrop`, `SetBrushForXamlRoot` |
| [`WinUIBackdropConstants.cpp`](../GlassCore/src/Hooks/xaml/WinUIBackdropConstants.cpp) | Табличные RVA (`XamlMuxRvaSet`) по версии `Microsoft.UI.Xaml.dll`, выбор набора `SelectMuxRvaSet` |

Ключевая трассировка:

- **`[GlassCore][WinUIBackdrop][timeline]`** — общий монотонный **`seq`** для сопоставления порядка вызовов (`OnTargetConnected`, DComp, `put_CompositionBrush`, `SetBrushForXamlRoot`).
- После `OnTargetConnected` при пустом `get_SystemBackdrop` на объекте поддержки — резолв **Compositor** для отложенного `put_SystemBackdrop`: QI **`ICompositionObject`** на переданные указатели, затем альтернатива **`IXamlRoot::get_Content`** → **UIElement** → **`ICompositionObject`** → **`get_Compositor`** (vtable). IID: `Microsoft.UI.Xaml.IXamlRoot` и `Windows.UI.Xaml.IXamlRoot` в [`GlassShared/include/MicrosoftUICompositionIIDs.h`](../GlassShared/include/MicrosoftUICompositionIIDs.h), слот `get_Content` = **6** (проверка: `WinmdGuidDump --vtable … Microsoft.UI.Xaml.IXamlRoot`).
- Для меню **A**: **Compositor часто становится доступен только из следующего `put_CompositionBrush`** (`CompositionEffectBrush` на узле Acrylic и т.п.); поддерживается несколько активных **`ContentExternalBackdropLink`** до успешного `put_SystemBackdrop` по списку (важно для **второго** открытия).

### Результаты тестов: меню A — первое и второе открытие (2026-05-07, уточнено 2026-05-08)

Платформа **Debug x64**, **explorer.exe**, лог через LogViewer. Между «первым» и «вторым» открытием — полный dismiss, затем снова то же корневое контекстное меню. Ниже — **итог smoke**; с 2026-05-08 зафиксирована и **регрессия** второго открытия A при определённой цепочке MUX (см. следующий раздел).

| Открытие | Наблюдение по логу | Наблюдение по UI |
|---------|---------------------|-------------------|
| **Первое** | Два вызова `OnTargetConnectedImpl` подряд; **`compositionSupports`** — **`ContentExternalBackdropLink`**; при пустом `get_SystemBackdrop` — очередь отложенного put (`AppendDeferredBackdropLink`), лог **`[deferQ] После OTC append`**; далее обычно два **`put_CompositionBrush`** на **одном и том же `AcrylicBrush`**: сначала **`CompositionColorBrush`**, затем **`CompositionEffectBrush`**; на втором put **`get_SystemBackdrop` на link уже не пуст** → успешная подмена / **`Отложенный put_SystemBackdrop … linkIndex=…`** | Кастомная кисть на link применяется |
| **Второе (успешный вариант)** | Как в строке выше для первого: если снова есть **ColorBrush → EffectBrush**, повторяется сценарий с непустым `get` и отложенным put | Визуал как при первом открытии |
| **Второе (регрессия, зафиксировано 2026-05-08)** | Тот же **`pThis` Acrylic**, **новые** указатели **link**, очередь **`links=2`**, **тот же TID**; но MUX шлёт **сразу только `CompositionEffectBrush`** (нет отдельного put **ColorBrush**). Включается **defer-pre** («без Put и без TryCreateReplacement при пустом get») → на обоих link **`hr=0x80004002` (ENIF)** → ветка **«только ENIF/S_FALSE — passthrough MUX EffectBrush (без priming; wuceffectsi)»** | Кастомная кисть на **link** не применяется (MUX EffectBrush проходит как есть) |

#### Матрица вынесена в отдельный файл

Чтобы не раздувать этот документ, вся рабочая матрица вариантов по второму открытию меню A перенесена в:

- `ExplorerXamlPopupAttemptMatrix.md`

Там же фиксируются новые прогоны по B-линейке (начиная с B1).

**Критерий успеха (уточнённый):** для меню A признак «здорового» первого открытия — пара put **ColorBrush** + **EffectBrush** на одном acrylic и успешный отложенный put по одному из link. Для второго открытия при **только EffectBrush** на reuse-acrylic ожидаема ветка defer-pre/passthrough — это не гонка потоков и не пустая очередь (в логах **`[deferSnap] links=2`**, **`waitingCompositor=1`**).

---

## Наблюдения WinUIBackdrop + XAML Diagnostics (2026-05-08)

Сводка по **реальным логам** (LogViewer): корреляция **GlassCore** `[WinUIBackdrop]` с **ExplorerCompat** `[XamlTree]` / `[XamlDiag]`, без изменения кода в этом документе.

### Две копии `Microsoft.UI.Xaml.dll` в одном `explorer.exe`

В логе инициализации: сначала может подгрузиться **SystemApps CBS 2.9.x** (`…\Microsoft.UI.Xaml.CBS_…\Microsoft.UI.Xaml.dll`) — **частичный Detour** (часто только `put_CompositionBrush`, **`onTarget=0`** и т.д. в строке `Detour OK`). Позже подключается **Windows App Runtime 3.1.5** (`…\Microsoft.WindowsAppRuntime.CBS_…\Microsoft.UI.Xaml.dll`) — **полный** набор хуков включая **`OnTargetConnectedImpl`**. Политика в коде: **RVA/IDA ориентиры под WASR**; вторая копия без отдельного набора RVA **не патчится** так же полно. Риск: часть вызовов теоретически может идти через **непатченный** образ — при разборе крашей в WinDbg сверять **`lm m Microsoft.UI.Xaml`** и кадр возврата из thunk.

### Меню A: что опровергли логи

- **Не подтверждена** гипотеза «OTC и `put_CompositionBrush` на разных потоках → пустой снимок очереди»: в проблемных прогонах **`tid` один** (**например 46808 / 50156 / 55240**), **`[deferSnap] links=2`**, **`waitingCompositor=1`**.
- **Подтверждено**: при втором показе **переиспользуется тот же `AcrylicBrush`** (**одинаковый `pThis_Acrylic`**), создаются **новые** `ContentExternalBackdropLink` (**новые `compositionSupports`**).
- **Ключевой различитель первого и второго открытия A**: порядок MUX на acrylic — **есть ли** отдельный put **`CompositionColorBrush` перед** **`CompositionEffectBrush`**. Если **есть** — на момент EffectBrush **`get_SystemBackdrop` на link часто уже не пуст**, отложенный put и подмена срабатывают. Если **нет** (сразу EffectBrush) — срабатывает защитный **defer-pre** (стабильность **`wuceffectsi`**) → ENIF на pre по link → **passthrough без priming** → визуально **нет кастомной кисти на link**.
- **Платформа без перезапуска explorer**: тот же механический путь (defer-pre → passthrough), если нет «холодного» ColorBrush; визуально меню может не получить кисть — это согласовано с описанной веткой кода, а не обязательно «рестарт Glass».

### XAML Diagnostics (дерево): что добавляет картине

- События **`[ExplorerCompat][XamlTree]`** показывают, что **первое и второе открытие** используют **одну и ту же структуру** (например **`FlyoutPresenter`**, **`CommandBarFlyoutCommandBar`**, сетки **`PrimaryItemsSystemBackdropRoot`**, **`OverflowPopupSystemBackdropRoot`**). Регрессия второго A — **не** из‑за «другого типа попапа» в дереве.
- По времени: **OTC** фиксируется **до** появления **`PrimaryItemsSystemBackdropRoot`** в выборке VTC; первый значимый **`put_CompositionBrush`** — **позже** по меткам времени; во **втором** показе первый put часто **сразу EffectBrush** — согласуется с текстовой трассировкой WinUIBackdrop.

### Встроенная диагностика GlassCore (Debug)

- **`[deferQ]`** (`LogDeferredBackdropQueueStats` в `WinUIBackdropState.cpp`): после **`AppendDeferredBackdropLink`** — **tid**, указатель link, **`queueSize`**, флаг **`waitingCompositor`** под тем же mutex, что и очередь.
- **`[deferSnap]`**: сразу после **`CopyDeferredBackdropComposeState`** в **`Hooked_XamlCompositionBrushBase_put_CompositionBrush`** — **seq**, **tid**, **pid**, **`links`**, **`waitingCompositor`**, **`pThis_Acrylic`**.
- **WARN** «очередь link пуста при ожидании defer (EffectBrush)» — если **`waitingCompositor`**, пустые **`links`**, входящий brush — EffectBrush (в наблюдённых успешных сценариях с **links=2** не срабатывает).

### Меню B стабильно получает кастомную кисть (при том что A «ломается» на втором цикле)

**Важно:** речь о пути **GlassCore Detours** (`WinUIBackdrop`), а не о том, что меню B «не патчится» через **XAML Diagnostics / VTS** (там по-прежнему свои ограничения на создание backdrop через `CreateInstance`).

Наблюдаемая **разница каналов**:

| | Меню A (корневое CommandBar flyout) | Меню B (подменю; типичная трассировка) |
|---|-------------------------------------|----------------------------------------|
| **`compositionSupports` в OTC** | **`Microsoft.UI.Content.ContentExternalBackdropLink`** | **`Microsoft.UI.Xaml.Controls.Primitives.Popup`** |
| **Отложенная очередь link** | Да: Compositor из XAML в OTC не резолвится → **`AppendDeferredBackdropLink`**, ожидание **`put_CompositionBrush` с EffectBrush** | Нет ветки «два ExternalBackdropLink + defer»: лог **«без отложенного put_SystemBackdrop»** на link |
| **Ключевой успешный путь** | OTC + позже **Acrylic** `put` + **`TryReapply`/`put_SystemBackdrop` на link** (зависит от ColorBrush→EffectBrush) | **`Popup::put_SystemBackdrop`** / вложенный **`CPopup::SetSystemBackdrop`** с подменой на **`CompositionBackdropBrush`** (Windows.UI ABI), без той же defer-цепочки |

### Сценарий чередования A и B (лог 2026-05-08)

Последовательность: **Open A → кисть ОК → Open B → кисть ОК → Close → Open A → кисть skip → Open B → кисть ОК**.

- Первое **A**: `seq` 1–2 OTC (link), `seq=3` **ColorBrush**, `seq=4` **EffectBrush**, успешный отложенный put / **`put_SystemBackdrop подмена применена`**.
- **B**: `seq=5` OTC с **`Popup`**, **`Popup::put_SystemBackdrop`** (`seq=6`), **`CPopup::SetSystemBackdrop`** с **`CompositionBackdropBrush`**; **`put_CompositionBrush(nullptr)`** на acrylic (`seq=10`) — сброс после закрытия A/B.
- Второе **A**: `seq=11–12` OTC (новые link), **`seq=13` только EffectBrush** → defer-pre → ENIF на обоих link → **passthrough**.
- Второе **B**: снова `Popup` / **`Popup::put_SystemBackdrop`** — путь как в первом **B**, кисть применяется.

Вывод: **стабильность B** связана с тем, что подмена идёт через **прямую цепочку Popup/SystemBackdrop**, **минуя** хрупкий сценарий **«два новых ExternalBackdropLink + пустой get + defer-pre на reuse-Acrylic»**.

### Инструменты для дальнейшей локализации

- **LogViewer + Debug GlassCore** — уже даёт полную корреляцию seq/тид/очереди/acrylic ptr.
- **WinDbg** при AV в **`wuceffectsi.dll`**: `.ecxr`, `lm m wuceffectsi;m Microsoft.UI.Xaml`, стек до/после thunk **`put_CompositionBrush`**.
- **IDA** на **том же** `Microsoft.UI.Xaml.dll` (WASR), что в логе attach: xref с **`put_CompositionBrush`** — кто инициирует **ColorBrush перед EffectBrush** при первом показе и почему пропускает при reuse.
- **Цепочка `OnTargetConnected` / `put_SystemBackdropImpl` / `InvokeOnTargetConnected`** (таблица xref, псевдокод, заметки по IDA 9.x): [IdaMicrosoftUiXamlSystemBackdropCallChain.md](IdaMicrosoftUiXamlSystemBackdropCallChain.md).
- **`WinmdGuidDump --vtable`** — проверка слотов **`ICompositionSupportsSystemBackdrop`** после обновления WASR.

---

## Важно для будущего

**НЕ УДАЛЯТЬ рабочий подход `Visibility=Collapsed`!**

Если backdrop снова "потерялся":
1. Проверь, что `SystemBackdropRoot` обрабатывается в `OnVisualElementAdded`
2. Проверь, что применяется `Visibility=Collapsed` через `CreateInstance` + `SetProperty`
3. Проверь логи: должно быть `SystemBackdropRoot.Visibility=Collapsed: handle=... hr=0x00000000`
4. Если `hr != 0x00000000` — проверь, что `CreateInstance` успешно создал enum `Microsoft.UI.Xaml.Visibility.Collapsed`

**Изменение цвета/альфа backdrop — это отдельная задача**, которая требует других методов (reverse engineering внутренних API WinUI 3). Отключение видимости backdrop работает и должно оставаться в коде.
