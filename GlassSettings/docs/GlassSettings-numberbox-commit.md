# GlassSettings — commit для NumberBox

Единый контракт ручного числового ввода на detail-страницах. Код:
[`SettingsNumberBoxCommit`](../../GlassSettings/Services/SettingsNumberBoxCommit.cs).

## Поведение

- ввод и вставка текста остаются pending до нажатия `Enter`;
- spin-кнопки и клавиши `Up` / `Down` / `PageUp` / `PageDown` записывают значение сразу;
- `Enter` нормализует и записывает pending-ввод;
- при потере фокуса системный `NumberBox.ValueChanged` также проходит общую нормализацию
  диапазона и шага, затем записывает значение;
- перед первым символом ручного редактирования сервис кэширует текущее значение;
  `Escape` восстанавливает именно этот кэш, снимает фокус и подавляет `ValueChanged`,
  поэтому запись параметра, dirty, notice и live reload не вызываются;
- перед записью значение ограничивается `Minimum..Maximum` и округляется к ближайшему
  `Minimum + N * SmallChange`; середина округляется вверх;
- после программного `Reload` страница обязана вызвать `SyncCommittedValue`, чтобы отменить
  pending-ввод и принять загруженное значение как baseline.

Обычный NumberBox, как и live-слайдер, работает **вне** `RunSequenceAsync`: page callback
сохраняет штатные gate/loading guards, запись реестра, live reload, dirty и notice.

## Подключение

```csharp
SettingsInteractionDeferral.WhenNumberBoxCommitOnEnter(box, parameterPanel);
SettingsNumberBoxCommit.Attach(
    box,
    canCommit: () => !_loading && expander.IsGateOn,
    onCommit: value => WriteParameter((uint)value));
```

После загрузки значения:

```csharp
box.Value = ReadParameter();
SettingsNumberBoxCommit.SyncCommittedValue(box, box.Value);
```

Если другое действие читает текущее значение NumberBox (например, color picker сохраняет
цвет вместе с threshold), перед чтением нужно вызвать `SettingsNumberBoxCommit.Flush(box)`.

## Диапазоны страниц

- Popup animation: времена `0..5000`, StartRatio `0..100`, шаг `1`;
- Menu item: CornerRadius `0..10`, шаг `1`; Width `0..10000`, шаг `1000`;
- Tooltip margins: `0..1000`, шаг `1`;
- Menu и Root icon threshold: `0..510`, шаг `1`;
- DWM: RoundRectRadius `0..50`, TextGlowSize `0..100`, шаг `1`.

Popup-диапазоны сверены с `TranslucentFlyoutsConfig-master`, DWM — с
`OpenGlassGUI`. Для времен сохранён шаг GlassSettings `1`: donor-step `100`
не образует допустимую сетку для штатных значений `350`, `250` и `87`.

## Anti-patterns

- сырой `NumberBox.ValueChanged` с записью параметра на странице;
- повторное чтение mutable `NumberBox.Value` внутри отложенного callback вместо переданного
  нормализованного значения;
- программный `Reload` без `SyncCommittedValue`;
- локальная обработка Enter или локальное округление в каждой странице.
