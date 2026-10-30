# GlassSettings — локализация строк

Все **пользовательские** строки интерфейса GlassSettings хранятся в PRI-ресурсах и загружаются **только** через `GlassSettingsResources`. Литералы в `.cs` и `.xaml` для UI **запрещены** (исключение — технические идентификаторы вроде имён ключей реестра, если они сознательно показываются как есть).

---

## Языки

| Каталог | Язык |
|---------|------|
| `GlassSettings/Strings/ru-RU/Resources.resw` | Русский (язык по умолчанию) |
| `GlassSettings/Strings/en-US/Resources.resw` | English |

В `GlassSettings.csproj`:

- `DefaultLanguage` = `ru-RU`
- `SupportedLanguages` = `ru-RU;en-US`

Язык интерфейса определяется **локалью Windows** пользователя, если в настройках приложения выбран **«Системный»**. Явный выбор сохраняется в реестре `HKCU\Software\StanislavKa\GlassPlatform\GlassSettings\AppUiLanguage` как **строка BCP-47** (`ru-RU`, `en-US`, …); отсутствие ключа — системный язык. В коде: `AppUiLanguage.RuRu` / `AppUiLanguage.EnUs` (BCP-47), `AppUiLanguage.LangIdRuRu` / `LangIdEnUs` (LANGID). Список языков в ComboBox — из каталогов `Strings/<tag>/Resources.resw` (генерация MSBuild + проверка PRI в `UiLanguageCatalog`).

Переключатель — диалог настроек приложения (иконка шестерёнки в title bar): карточка **«Язык интерфейса»** справа от темы и backdrop.

---

## API

Используется **`Microsoft.Windows.ApplicationModel.Resources.ResourceLoader`** (MRT Core, WinApp SDK).  
**Не** `Windows.ApplicationModel.Resources` — in-box MRT для UWP в unpackaged-приложении (`WindowsPackageType=None`) падает при инициализации.

Рядом с `GlassSettings.exe` должен лежать **`GlassSettings.pri`** (генерируется сборкой).

```csharp
using GlassSettings.Strings;

// Простая строка
var title = GlassSettingsResources.Get("NavHome");

// Формат ({0}, {1}, …)
var line = GlassSettingsResources.Format("Home_DeviceHeaderWithOs", osCaption);

// Плюрализация 1 / 2–4 / 5+ (отдельные ключи *_One, *_Few, *_Many)
var text = GlassSettingsResources.CountForm(count, "Attach_Process_One", "Attach_Process_Few", "Attach_Process_Many");

// Пункты упорядоченного списка (комбобокс, меню) — одна строка через «;»
var cornerLabels = GlassSettingsResources.GetSemicolonList("Combo_CornerTypeItems");
```

- **`Get(key)`** — единственный допустимый способ чтения строк из кода. Если ключ отсутствует, выбрасывается `InvalidOperationException` (ошибка сборки/ресурсов, не fallback).
- **`GetSemicolonList(key)`** — разбор списковых ключей `Combo_*` (см. раздел «Списковые элементы»).
- **Fallback-литералы в коде запрещены** — не дублировать русский текст «на случай отсутствия ключа».

---

## Именование ключей

Префикс по области, PascalCase, без пробелов:

| Префикс | Примеры |
|---------|---------|
| `AppTitle`, `Cmd*` | окно, command bar |
| `Nav*` | боковая навигация |
| `Breadcrumb_*` | заголовки drill-down |
| `Home_*` | страница «Обзор» |
| `Status_*` | статусы платформы / OpenGlass |
| `Custom_*`, `Effect_*`, `Param_*` | учёт кастомизации, эффекты |
| `Combo_*` | пункты списков (комбобоксы, enum-like меню) — одна строка на список |
| `Attach_*`, `Themes_*` | процессы, визуальные стили |
| `Dialog_*` | Hosted-диалоги и подтверждения |
| `Dwm_*`, `Glass_*`, `Flyout_*` | detail-страницы |
| `System_*` | страница «Система» |
| `Placeholder_*` | заглушки разделов |
| `Toggle_*` | подписи переключателя |

Один смысл — **один ключ**. Заголовок раздела в nav, breadcrumb и на «Обзоре» должен ссылаться на **тот же** ключ (например `NavThemes`), а не на три разных литерала.

---

## XAML

Текст в разметке **не хардкодить**. После `InitializeComponent()` вызывать `ApplyLocalizedStrings()` (или аналог) и задавать `Text`, `Title`, `Description`, `PlaceholderText`, `ToolTip` из `GlassSettingsResources`.

`x:Uid` / `ms-appx:` для новых экранов не использовать — единый путь через `GlassSettingsResources`.

---

## Списковые элементы (комбобоксы, меню и т.п.)

Пункты **упорядоченного** списка с привязкой к числовому индексу (DWORD в реестре, `SelectedIndex` комбобокса, пункт `MenuFlyoutItem` по порядку) хранятся **одной** строкой ресурса: значения через **`;`** в порядке индексов **0, 1, 2, …**

**Не** заводить отдельный ключ на каждый пункт (`Anim_SlideDown`, `Flyout_CornerRound`, `Effect_Blur`, …) и **не** перечислять `Items.Add(...)` в code-behind страниц.

### Чтение и заполнение UI

```csharp
// Разбор списка
var items = GlassSettingsResources.GetSemicolonList("Combo_CornerTypeItems");

// Заполнение ComboBox — через helper в Core/Flyout, не в Page
FlyoutCornerNames.PopulateCombo(_cornerCombo);
```

- **`GlassSettingsResources.GetSemicolonList(key)`** — единственный способ split: `';'`, `TrimEntries`, пустые сегменты отбрасываются.
- Индекс в UI **строго** совпадает с индексом в реестре / enum.
- Логику «имя списка → PopulateCombo / Get(index)» выносить в `GlassSettings/Core/Flyout/*Names.cs` (или аналог по домену), а не дублировать на страницах.

### Именование ключей

Префикс **`Combo_`**, суффикс **`Items`**:

| Ключ | Назначение |
|------|------------|
| `Combo_EffectTypeItems` | подписи в комбобоксе EffectType (с `(SWCA)` / `(DWM)` где нужно) |
| `Combo_EffectTypeNames` | короткие имена того же порядка — для Обзора, `CustomizationValueFormatter`, `FlyoutEffectNames.Get` |
| `Combo_CornerTypeItems` | CornerType (0–3) |
| `Combo_PopInStyleItems` | PopInStyle анимации flyout (0–3) |

Если текст в комбобоксе совпадает с текстом в других местах UI — **достаточно одного** `Combo_*Items`. Второй ключ (`Combo_*Names`) — только когда подпись в списке намеренно длиннее (суффиксы backend, уточнения).

Пример (ru-RU):

```xml
<data name="Combo_CornerTypeItems" xml:space="preserve">
  <value>По умолчанию;Прямые углы;Крупное скругление;Мелкое скругление</value>
</data>
```

Тот же порядок и количество сегментов — в **en-US**. Менять порядок в одной локали без другой **нельзя** (ломается соответствие индекс ↔ реестр).

### Тот же паттерн вне комбобокса

Строки «несколько подстановок в одном ключе» через `;` допустимы и для **не-combo** текстов, если порядок сегментов задаётся кодом по индексу. Пример: `Flyout_EffectScopeTargets` → `FlyoutEffectScopeDescriptions` (split + `Format`).

### Анти-пatterns

| Нельзя | Вместо этого |
|--------|----------------|
| `combo.Items.Add(GlassSettingsResources.Get("Anim_Ripple"))` в цикле/вручную | `FlyoutAnimNames.PopulatePopInStyleCombo(combo)` |
| Массив строковых ключей `Effect_*` в C# | один `Combo_EffectTypeItems` в `.resw` |
| Разный порядок пунктов в ru и en | одинаковое число сегментов, тот же индекс = тот же смысл |
| Split `;` в каждом месте ad hoc | `GetSemicolonList` или helper `*Names` |

---

## Добавление строки

1. Добавить ключ в **оба** файла: `ru-RU/Resources.resw` и `en-US/Resources.resw`.
2. В коде/XAML использовать только `GlassSettingsResources.Get` / `Format` / `CountForm`.
3. Проверить UTF-8 (не UTF-16): [file-encoding.md](../contributing/file-encoding.md).
4. Собрать GlassSettings x64 Debug и убедиться, что ключ не падает в runtime.

Для **списковых** ключей `Combo_*` — см. раздел «Списковые элементы» выше: один ключ на список, обе локали, helper в `Core/Flyout/`.

Для плюрализации в русском — три ключа (`*_One`, `*_Few`, `*_Many`); в английском `*_Few` и `*_Many` часто совпадают.

---

## Связанные документы

- [GlassSettings-development.md](GlassSettings-development.md) — общая разработка UI
- [file-encoding.md](../contributing/file-encoding.md) — кодировка `.resw`
