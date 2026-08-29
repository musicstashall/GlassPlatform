# План: Стиль компановки Windows 10 на странице «Панель задач»

## Цель
Добавить на страницу «Панель задач» (Win11 only) карточку с тумблером «Стиль компановки Windows 10». При включении панель задач перестраивается в стиль Win10 (Пуск слева, без виджетов), при выключении — возвращается к компановке Win11 по умолчанию.

## Ограничения
- Настройка доступна только на Windows 11 (`_isWin11`).
- Настройка включает два независимых параметра:
  1. **Выравнивание панели** — `TaskbarAl` (`HKCU\...\Explorer\Advanced`), **не блокируется UCPD**.
  2. **Виджет на панели** — `TaskbarDa` (`HKCU\...\Explorer\Advanced`), **блокируется UCPD** на Win11 24H2+.
- Для обхода блокировки UCPD одновременно записываем **policy-ключ** `HKCU\Software\Policies\Microsoft\Dsh\AllowNewsAndInterests` (документированный аналог GPO «Allow widgets»), который UCPD не блокирует.
- Изменения применяются мгновенно (broadcast `WM_SETTINGCHANGE`), без перезапуска explorer.
- `TaskbarDa` + `AllowNewsAndInterests` отслеживаются через registry watch с уведомлением и зеленым зеркалом (external-sync notice).

## Файлы для изменения

### 1. Локализация
- `GlassSettings/Strings/ru-RU/Resources.resw`
- `GlassSettings/Strings/en-US/Resources.resw`

Добавить ключи:
- `Flyout_Win10Layout` — «Стиль компановки Windows 10» / «Windows 10 layout style»
- `Flyout_Win10LayoutDesc` — «Возвращает кнопку Пуск в левый угол и скрывает кнопку виджетов» / «Returns the Start button to the left and hides the Widgets button»

Существующие ключи для external-sync notice (не добавлять):
- `Notice_ExternalSectionSynced` — уже есть в обоих языках.

### 2. Иконки секций
- `GlassSettings/Controls/SettingsSectionGlyphs.cs`

Добавить `SectionKind.Taskbar` (глиф `\uE7C4`) в enum и в `ForKind`.

### 3. XAML страницы
- `GlassSettings/Pages/Glass/TaskbarFlyoutPage.xaml`

После `ZonesRow` добавить:
```xml
<ctrl:SettingsOptionRow x:Name="Win10LayoutRow" />
```

### 4. Новый сервис наблюдения за реестром
- `GlassSettings/Services/TaskbarRegistryWatchService.cs` (новый файл)

Следует паттерну `WindowMetricsRegistryWatchService`:
- Управляет двумя `RegistryValueChangeWatcher`:
  1. `HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced` → `TaskbarDa`
  2. `HKCU\Software\Policies\Microsoft\Dsh` → `AllowNewsAndInterests`
- Debounce 450 ms
- Единое событие `ExternalRegistryChanged` (срабатывает при изменении любого из двух ключей)
- Suppress при наших записях (`SuppressExternalDirty`)
- Start/Stop lifecycle
- Если ключ `Dsh` или `AllowNewsAndInterests` отсутствуют в реестре, watcher для него не стартует (ограничение `RegNotifyChangeKeyValue`). Изменение обнаруживается при следующем `Reload()`.

### 5. Модификация SettingsSession
- `GlassSettings/Services/SettingsSession.cs`

Добавить метод:
```csharp
internal void NotifyExternalRegistrySync(SettingsCommitScope scope)
{
    LastDirtyChangedSource = DirtyChangedSource.RegistryWatchSynced;
    PageExternalSyncNoticeState.MarkScopesSynced([scope]);
    RaiseDirtyChanged();
}
```

### 6. Код страницы
- `GlassSettings/Pages/Glass/TaskbarFlyoutPage.xaml.cs`

#### Новые поля
```csharp
private readonly GlassSettingsLabeledToggleSwitch _win10LayoutSwitch = new();
private TaskbarRegistryWatchService? _taskbarWatch;
```

#### Конструктор
- `Win10LayoutRow.HeaderActions = _win10LayoutSwitch;`
- `Win10LayoutRow.DimHeaderActionsWhenInactive = false;`
- `Win10LayoutRow.Visibility = _isWin11 ? Visibility.Visible : Visibility.Collapsed;`
- `Win10LayoutRow.IconGlyph = SettingsSectionGlyphs.ForKind(SettingsSectionGlyphs.SectionKind.Taskbar);`
- `_win10LayoutSwitch.Toggled += Win10LayoutSwitch_OnToggled;`

#### Локализация (`ApplyLocalizedStrings`)
- `Win10LayoutRow.Title = GlassSettingsResources.Get("Flyout_Win10Layout");`
- `Win10LayoutRow.Description = GlassSettingsResources.Get("Flyout_Win10LayoutDesc");`

#### Loaded / Unloaded
- В `Loaded`: 
  - Создать ключ `HKCU\Software\Policies\Microsoft\Dsh` если отсутствует (чтобы watcher мог мониторить `AllowNewsAndInterests`).
  - Создать `_taskbarWatch`, подписаться на `ExternalRegistryChanged`, запустить.
- В `Unloaded`: отписаться, остановить, освободить.

#### Reload
После `SyncSeparateZonesUi(paintXaml);` добавить `SyncWin10LayoutUi();`.

#### Реализовать `IExternalRegistrySyncReloadable`
```csharp
public void ReloadFromExternalRegistrySync() => Reload();
```

#### Новые методы
- `SyncWin10LayoutUi()` — читает `TaskbarAl`, `TaskbarDa` и `AllowNewsAndInterests`, синхронизирует `_win10LayoutSwitch.IsOn`. 
  - Win10 layout = (`TaskbarAl == 0`) И (виджеты скрыты = `TaskbarDa == 0` ИЛИ `AllowNewsAndInterests == 0`).
  - При отсутствии ключей — дефолт Win11 (`TaskbarAl=1`, виджеты видны).
- `Win10LayoutSwitch_OnToggled(...)` — записывает `TaskbarAl` и оба ключа виджетов, подавляет watcher на 600 ms, отправляет broadcast, вызывает `SyncWin10LayoutUi()`.
- `ReadTaskbarAl()` / `WriteTaskbarAl(uint value)` — работа с `HKCU\...\Explorer\Advanced\TaskbarAl`.
- `ReadTaskbarDa()` / `WriteTaskbarDa(uint value)` — работа с `HKCU\...\Explorer\Advanced\TaskbarDa`.
- `ReadAllowNewsAndInterests()` / `WriteAllowNewsAndInterests(uint value)` — работа с `HKCU\Software\Policies\Microsoft\Dsh\AllowNewsAndInterests`.

#### Обработчик watcher
```csharp
private void OnTaskbarRegistryChanged()
{
    // TaskbarDa/AllowNewsAndInterests не входят в BWM snapshot,
    // поэтому HasUnsavedInScope(BarPageScope) проверяет только Tray-параметры GS.
    // Если пользователь не редактирует другие параметры панели — external-sync notice показываем.
    if (SettingsSession.Current.HasUnsavedInScope(TaskbarScopeCatalog.BarPageScope))
        return;

    SettingsSession.Current.NotifyExternalRegistrySync(TaskbarScopeCatalog.BarPageScope);
}
```

#### Верификация записи
После каждой записи ключа рекомендуется read-back для подтверждения:
```csharp
private void VerifyWrite(string valueName, uint expected)
{
    var actual = valueName switch
    {
        "TaskbarAl" => ReadTaskbarAl(),
        "TaskbarDa" => ReadTaskbarDa(),
        "AllowNewsAndInterests" => ReadAllowNewsAndInterests(),
        _ => 1u
    };
    if (actual != expected)
        throw new InvalidOperationException($"Registry write verification failed for {valueName}: expected {expected}, got {actual}");
}
```

## Детали реализации

### Реестр — три ключа

| Параметр | Путь | Значение | Блокировка UCPD |
|-----------|------|----------|-----------------|
| Выравнивание | `HKCU\...\Explorer\Advanced\TaskbarAl` | `0`=слева, `1`=центр | **Нет** |
| Виджеты (pref) | `HKCU\...\Explorer\Advanced\TaskbarDa` | `0`=скрыт, `1`=виден | **Да** (24H2+) |
| Виджеты (policy) | `HKCU\Software\Policies\Microsoft\Dsh\AllowNewsAndInterests` | `0`=не разрешены, `1`=разрешены | **Нет** |

### Семантика ключей

- `TaskbarAl` — обычный пользовательский preference, изменяемый через Settings → Taskbar alignment.
- `TaskbarDa` — preference для кнопки виджетов. **Блокируется UCPD** начиная с августа 2024 (KB-обновления) и Win11 24H2+.
- `AllowNewsAndInterests` — policy-ключ, соответствует GPO «Computer Configuration → Administrative Templates → Windows Components → Widgets → Allow widgets». **UCPD не блокирует** policy-ключи.
- При конфликте: policy (`AllowNewsAndInterests`) имеет приоритет над preference (`TaskbarDa`).

### Запись при переключении тумблера

```csharp
private void Win10LayoutSwitch_OnToggled(object sender, RoutedEventArgs e)
{
    if (_loading) return;

    var enableWin10Layout = _win10LayoutSwitch.IsOn;
    var widgetsValue = enableWin10Layout ? 0u : 1u;
    
    // 1. Выравнивание (не блокируется UCPD)
    WriteTaskbarAl(enableWin10Layout ? 0u : 1u);
    
    // 2. Виджеты — preference (может быть заблокирован UCPD)
    try
    {
        WriteTaskbarDa(widgetsValue);
    }
    catch (UnauthorizedAccessException)
    {
        // UCPD блокирует TaskbarDa — ожидаемо на Win11 24H2+.
        // Продолжаем, т.к. policy-ключ ниже.
    }
    
    // 3. Policy-ключ (UCPD не блокирует) — основной механизм на 24H2+
    WriteAllowNewsAndInterests(widgetsValue);
    
    // 4. Подавляем watcher на собственную запись
    _taskbarWatch?.SuppressExternalDirty(TimeSpan.FromMilliseconds(600));
    
    // 5. Broadcast
    SendNotifyMessageW(HWND_BROADCAST, WM_SETTINGCHANGE, IntPtr.Zero, "Environment");
    
    SyncWin10LayoutUi();
}
```

### Чтение для UI

```csharp
private bool IsWin10LayoutActive()
{
    var taskbarAl = ReadTaskbarAl();
    var taskbarDa = ReadTaskbarDa();
    var allowNews = ReadAllowNewsAndInterests();
    
    // Win10 layout: left alignment + widgets hidden
    var widgetsHidden = taskbarDa == 0 || allowNews == 0;
    return taskbarAl == 0 && widgetsHidden;
}

private void SyncWin10LayoutUi()
{
    if (!_isWin11)
    {
        Win10LayoutRow.Visibility = Visibility.Collapsed;
        return;
    }
    Win10LayoutRow.Visibility = Visibility.Visible;
    _win10LayoutSwitch.IsOn = IsWin10LayoutActive();
}
```

### Broadcast
```csharp
const uint WM_SETTINGCHANGE = 0x001A;
const IntPtr HWND_BROADCAST = new(0xFFFF);

SendNotifyMessageW(
    HWND_BROADCAST,
    WM_SETTINGCHANGE,
    IntPtr.Zero,
    "Environment");
```

P/Invoke декларация (аналог `SingleInstance.cs`):
```csharp
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
private static extern IntPtr SendNotifyMessageW(
    IntPtr hWnd, uint Msg, IntPtr wParam, string lParam);
```

### UnsavedScope / External-sync
`Win10LayoutRow.UnsavedScope = TaskbarScopeCatalog.BarPageScope;`

Ключи `TaskbarDa`/`TaskbarAl`/`AllowNewsAndInterests` — системные, не входят в BWM snapshot. Отслеживаются через отдельный watcher. При внешнем изменении:
1. `TaskbarRegistryWatchService` детектирует изменение любого из двух ключей.
2. Если нет несохранённых изменений на странице «Панель задач», вызывается `SettingsSession.Current.NotifyExternalRegistrySync(TaskbarScopeCatalog.BarPageScope)`.
3. `SettingsSession` ставит `LastDirtyChangedSource = RegistryWatchSynced`, помечает scope как синхронизированный, поднимает `DirtyChanged`.
4. `SettingsPageShell.OnDirtyChanged` видит флаг, вызывает `ReloadFromExternalRegistrySync()` на странице, показывает зеленое MirrorOnly notice (`Notice_ExternalSectionSynced`).

## Проверка
1. Собрать решение.
2. На Win11 открыть GlassSettings → «Панель задач».
3. Карточка «Стиль компановки Windows 10» видна, тумблер отражает текущее состояние.
4. Включить тумблер → панель задач перестраивается (Пуск слева, виджеты скрыты).
5. Выключить тумблер → панель возвращается к Win11 компановке.
6. На Win10 карточка отсутствует.
7. Изменить `TaskbarDa` из regedit → через 450 ms GS детектирует изменение, показывает зеленое notice, перезагружает страницу.
8. Изменить `AllowNewsAndInterests` из regedit → через 450 ms GS детектирует изменение, показывает зеленое notice.
9. Перезапустить GlassSettings — состояние тумблера соответствует актуальным ключам.

## Риски
- UCPD блокирует запись `TaskbarDa` на Win11 24H2+. Митигируем записью в `AllowNewsAndInterests` (policy key, не блокируется UCPD).
- Broadcast `WM_SETTINGCHANGE` может не вызывать мгновенной перестройки на некоторых сборках. Fallback: перезапуск explorer вручную.
- Если пользователь меняет ключи сторонними средствами, `Reload()` корректно подхватит новое значение.
- Watcher может детектировать собственное изменение если suppress не сработал — mitigate: 600 ms suppress + debounce 450 ms.

## Открытые вопросы
- Все решено.
