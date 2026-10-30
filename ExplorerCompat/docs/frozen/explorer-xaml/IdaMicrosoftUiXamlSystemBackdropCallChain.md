# IDA: цепочка `SystemBackdrop::OnTargetConnected` в `Microsoft.UI.Xaml.dll`

**Дата:** 2026-05-08  
**Бинарь:** `Microsoft.UI.Xaml.dll` из установки **Windows App Runtime** (на исследуемой машине тот же модуль, что в стеках/логах GlassCore; при обновлении WASR смещения и имена могут сдвинуться — логика цепочки типична).  
**Цель:** не повторять разведку с нуля: от `put_SystemBackdrop` / island-хостов до `OnTargetConnectedImpl`, плюс практические заметки по IDA 9.x.

Связанные документы: [ExplorerXamlPopupPatchingAttempts.md](ExplorerXamlPopupPatchingAttempts.md), [ExplorerXamlIslands.md](ExplorerXamlIslands.md).

---

## 1. Заметки по IDA (чтобы не теряться)

| Ситуация | Действие |
|----------|----------|
| Список из **60k+** функций | Не листать **Functions**. Использовать **View → Names** (**Shift+F4**), **Ctrl+F** по подстроке (`OnTargetConnected`, `SystemBackdrop`, `interface_forwarder`). |
| Нет **левой колонки адресов** | Часто открыт **Graph** (**Space**). **Space** ещё раз → **текстовый** листинг, адреса слева. |
| Функция **схлопнута** в одну строку `COLLAPSED FUNCTION` | Указать на строку → **Ctrl+NumPad+** раскрыть; без NumPad — **ПКМ** на строке, **Expand** / аналог. |
| **`X` «не то» / пусто** | В этой среде перекрёстные ссылки — **Ctrl+X** (а не одиночное **X**). Курсор — на **адресе** первой инструкции (колонка `.text:…`), не на операнде **`arg_0`** и не только на **`arg_0`**: иначе IDA показывает **ссылки на стековый слот** внутри функции. |
| Нужны **все входы в символ** | **Names** → **ПКМ** по имени → cross-references / jump to xref (формулировка зависит от версии). |
| Из **диалога xref** перейти к caller | **Двойной клик** или **Enter** по строке с **`call` / `jmp`** → курсор в листинге на соответствующей инструкции. |
| Вернуться назад | **Alt+Left** (история навигации). |
| **Ctrl+X** показывает только **`.rdata` / `vftable` / `dd rva`** | Это **указатели и vtable**, не прямые **`call`**. Публичный WinRT часто бьёт **косвенно** по vtable — искать **code**-строки (**`p`**) или подниматься от **`…::put_*Impl`**, где **`call`’ы в `.text`** уже есть. |

---

## 2. Цепочка вызовов (сверху вниз)

Уровни **от публичного задания backdrop** до реализации:

0. **Кто зовёт `DesktopWindowXamlSource::put_SystemBackdropImpl` (DWXS):**  
   - **`ctl::interface_forwarder<… IDesktopWindowXamlSource, DesktopWindowXamlSourceGenerated>::put_SystemBackdrop`** (**+0x13**) — путь **WinRT/COM** (внешний **`put_SystemBackdrop`** на интерфейсе).  
   - **`DirectUI::DesktopWindowImpl::put_SystemBackdropImpl`** (**+0x1C**) — обобщённая реализация для «десктопного» окна **делегирует** в **`DesktopWindowXamlSource::put_SystemBackdropImpl`**.

1. **`DirectUI::DesktopWindowXamlSource::put_SystemBackdropImpl`**  
   и параллельно **`DirectUI::XamlIsland::put_SystemBackdropImpl`**  
   — оба около **+0x163** от входа вызывают **`InvokeOnTargetConnected`** (похожий сгенерированный шаблон).

2. **`DirectUI::SystemBackdrop::InvokeOnTargetConnectedFromCore`** (**+0x7D**)  
   — внутренний путь из ядра XAML (**`CDependencyObject*`** в сигнатуре), тоже зовёт **`InvokeOnTargetConnected`**.

3. **`DirectUI::SystemBackdrop::InvokeOnTargetConnected`**  
   — тонкая обёртка: **`OnTargetConnectedProtected`** → проверка **`HRESULT`**, **`OnFailure_404_`** при ошибке.

4. **`DirectUI::SystemBackdropGenerated::OnTargetConnectedProtected`**  
   — развилка **`ctl::ComBase::IsComposed`**:  
   - **true:** **`QueryInterface`** → **`ISystemBackdropOverrides`** → **`pVirtuals->OnTargetConnected`** (vtable → **`interface_forwarder`**);  
   - **false:** прямой **`SystemBackdropGenerated::OnTargetConnected`**.

5. **`ctl::interface_forwarder<… ISystemBackdropOverrides, SystemBackdropGenerated>::OnTargetConnected`**  
   — thunk: подправка **`this`** (в т.ч. null-aware маска) и **`jmp`** на п.6.

6. **`DirectUI::SystemBackdropGenerated::OnTargetConnected`**  
   — телеметрия при **`Microsoft_Windows_XAMLEnableBits`**, проверки **`target` / `xamlRoot`**, **`DependencyObject::CheckThread`**, затем **`DirectUI::SystemBackdrop::OnTargetConnectedImpl`**.

7. **`DirectUI::SystemBackdrop::OnTargetConnectedImpl`**  
   — дальнейшая логика привязки (следующий уровень для IDA при необходимости).

---

## 3. Зафиксированные xref’ы (эталонный срез)

- **`SystemBackdropGenerated::OnTargetConnected` (entry):**  
  - **`interface_forwarder::OnTargetConnected` +0x10:** **`jmp`** (tail-call).  
  - **`OnTargetConnectedProtected` +0x85:** **`call`**.

- **`SystemBackdropGenerated::OnTargetConnectedProtected` (entry):**  
  - **`SystemBackdrop::InvokeOnTargetConnected` +0x6:** **`call`**.

- **`SystemBackdrop::InvokeOnTargetConnected` (entry):**  
  - **`DesktopWindowXamlSource::put_SystemBackdropImpl` +0x163:** **`call`**.  
  - **`XamlIsland::put_SystemBackdropImpl` +0x163:** **`call`**.  
  - **`InvokeOnTargetConnectedFromCore` +0x7D:** **`call`**.

- **`DesktopWindowXamlSource::put_SystemBackdropImpl` (entry), 2026-05-08:**  
  - **`ctl::interface_forwarder<ABI::Microsoft::UI::Xaml::Hosting::IDesktopWindowXamlSource, DirectUI::DesktopWindowXamlSourceGenerated>::put_SystemBackdrop` +0x13:** **`call`** — публичный путь **vtable / WinRT ABI** для **`IDesktopWindowXamlSource::put_SystemBackdrop`**, дальше сразу **`put_SystemBackdropImpl`**.  
  - **`DirectUI::DesktopWindowImpl::put_SystemBackdropImpl` +0x1C:** **`call`** — общая реализация «десктопного» окна передаёт работу конкретному **`DesktopWindowXamlSource::put_SystemBackdropImpl`**.

**Примеры VA на снимке сессии:**  
`OnTargetConnected` — **`0x1805B44E8`**; **`InvokeOnTargetConnected`** — **`0x180695990`**; **`call OnTargetConnectedProtected`** с листинга — **`0x1805B4689`**. На других сборках **не опираться на адрес**, только на **имена** и **смещения внутри функции** как ориентир.

---

## 4. Смысл псевдокода (Hex-Rays), кратко

### `interface_forwarder::OnTargetConnected`

Возврат **`SystemBackdropGenerated::OnTargetConnected(adjustedThis, …)`**, где **`adjustedThis`** — **`this`** со сдвигом к базе **`SystemBackdropGenerated`** и обнулением при **`this == nullptr`** (эквивалент **`neg/sbb/and`** в asm).

### `SystemBackdropGenerated::OnTargetConnected`

- Опционально **`XamlTelemetry::PublicApiCall`** при бите **`Microsoft_Windows_XAMLEnableBits`** (в образце проверка **`& 0x8000000`**).  
- **`OriginateError`** при **`nullptr`** для **`connectedTarget`** / **`xamlRoot`** (код в духе **E_INVALIDARG**, **`0x80070057`**).  
- **`DependencyObject::CheckThread`**.  
- **`SystemBackdrop::OnTargetConnectedImpl`**.  
- Ошибки → **`OnFailure_404_`**.  
- Повтор телеметрии на выходе при том же бите.

### `SystemBackdropGenerated::OnTargetConnectedProtected`

- Ветка **composed:** **`do_query_interface<ISystemBackdropOverrides>`** → **`Release`** в конце при ненулевом **`pVirtuals`**.  
- Ветка **не composed:** прямой вызов **`OnTargetConnected`**.  
- **`_mm_lfence`** перед **`OnFailure_404_`** на ошибочных путях (типичный mitigation-паттерн).

### `SystemBackdrop::InvokeOnTargetConnected`

Один вызов **`OnTargetConnectedProtected`**, **`HRESULT`**, **`OnFailure_404_`** при отрицательном коде.

### `DesktopWindowImpl::put_SystemBackdropImpl`

- **`DesktopWindowXamlSource::put_SystemBackdropImpl(this->m_desktopWindowXamlSource.ptr_, systemBackdrop)`** — та же цепочка, что уже разобрана (**`InvokeOnTargetConnected`** и ниже), но **`this`** острова/DWXS берётся из поля **`m_desktopWindowXamlSource`**.  
- **`_mm_lfence`**, при ошибке **`OnFailure_404_`**.  
- При успехе дополнительно **`DesktopWindowImpl::SetXamlIslandRootBackground(this, nullptr, systemBackdrop)`** — связывает **фон корня острова** с установленным **`ISystemBackdrop`**; при сбое — снова **`OnFailure_404_`**.  
- Итог: ветка **`DesktopWindowImpl`** — не отдельная реализация backdrop, а **обёртка: DWXS `put_SystemBackdropImpl` + правка root background**.

### `DesktopWindowImpl::SetXamlIslandRootBackground`

- Вызывается **с двух путей** (входящие **`call` в `.text`**):  
  **`DesktopWindowImpl::put_SystemBackdropImpl(MS.UI… ISystemBackdrop *)` +0x32** и **`DesktopWindowImpl::put_SystemBackdrop(Windows… ICompositionBrush *)` +0x3F** — общая точка для **нового** (`ISystemBackdrop`) и **старого/кистевого** (`ICompositionBrush`) сценария фона.  
- Логика (фрагмент Hex-Rays): взять **`m_pDO`** корня острова; при отсутствии **backdrop и brush** — сброс внутреннего флага и **`CPanel::NWSetBackgroundDirty`**; иначе **`Theming_TryEnableHostBackdropBrush(this->m_hwnd)`** и правка флага на **`m_pDO`**; ошибки — **`OnFailure_404_`**; финальный **`ComPtr::InternalRelease`** / **`Release`** по **`xamlIslandRoot`**.  
- **Вывод:** после успешного **`DWXS::put_SystemBackdropImpl`** хост **дополнительно** синхронизирует **Win32 HWND** (**`Theming_TryEnableHostBackdropBrush`**) и **dirty** панели корня острова.

**Импорт:** **`__imp_Theming_TryEnableHostBackdropBrush`** — функция **из внешней DLL** (IAT); в **`Microsoft.UI.Xaml.dll`** по **`Ctrl+X`** на импорте обычно видны **две строки на один и тот же `call`** (**`r`** / **`p`** — чтение слота IAT и сам вызов). Других caller’ов в этом модуле часто **нет** — смотреть **какой модуль экспортирует** символ: **`dumpbin /imports Microsoft.UI.Xaml.dll`** или точка останова в отладчике.

## 5. Что делать «дальше» при новом расследовании

1. Для **`DesktopWindowImpl` / `interface_forwarder::put_SystemBackdrop`:** **`Ctrl+X`** на entry часто даёт **только `.rdata` / vtable** — полный сценарий смотри **`F5`** (см. **`DesktopWindowImpl::put_SystemBackdropImpl`** выше). Дальше — искать property/сгенерированные вызовы или **`Ctrl+X`** у методов с **`call`** в **`.text`**.  
2. **`InvokeOnTargetConnectedFromCore`:** **F5** + **Ctrl+X** с entry — откуда дергает ядро (повторное подключение, смена root и т.д.).  
3. **`OnTargetConnectedImpl`:** **F5** и внутренние **`call`** — связка с **Composition** / **BackdropLink** (корреляция с логами GlassCore `defer-pre` / **`ContentExternalBackdropLink`**).
4. Аналогично для **`XamlIsland::put_SystemBackdropImpl`:** **Ctrl+X** с entry — обычно **`interface_forwarder`** для острова + свой **`…Impl`** родитель.

---

## 6. Вне репозитория

Скрипты отчёта по **MUX/WASR** (пример): `C:\Users\<user>\Documents\IDA-test\mux_ida_collect.py`, **`mux_ida_report.txt`** — не часть GlassProject; при переносе машины пути обновить вручную.
