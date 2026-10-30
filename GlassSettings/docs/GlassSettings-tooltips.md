# GlassSettings — тултипы (WinUI 3)

Acrylic-тултипы на полупрозрачном фоне окна (Mica). Строки UI — [GlassSettings-localization.md](GlassSettings-localization.md).

---

## Проблема WinUI 3

Стандартный `ToolTip` в WinUI 3 рисуется **сплошным** фоном; `AcrylicInAppFillColorDefaultBrush` в popup без `SystemBackdrop` даёт fallback-цвет без размытия. В WinUI 2 у tooltip был acrylic через UWP.

Implicit `Style` без `BasedOn="{StaticResource DefaultToolTipStyle}"` **ломает** шаблон: пропадают скругления и корректный layout.

`EventSetter` в WinUI 3 **не поддерживается** — хук `Loaded`/`Opened` только из code-behind.

---

## Файлы

| Файл | Назначение |
|------|------------|
| [`GlassSettings/Services/GlassSettingsToolTipService.cs`](../../GlassSettings/Services/GlassSettingsToolTipService.cs) | Единая точка `SetToolTip`; backdrop на popup |
| [`GlassSettings/Themes/GlassSettingsToolTip.xaml`](../../GlassSettings/Themes/GlassSettingsToolTip.xaml) | Стиль, шаблон без `FadeInThemeAnimation`, ресурсы `ToolTipBackgroundBrush` |
| [`GlassSettings/App.xaml`](../../GlassSettings/App.xaml) | Merged dictionary `Themes/GlassSettingsToolTip.xaml` |

---

## API

**Всегда** использовать `GlassSettingsToolTipService.SetToolTip`, не `ToolTipService.SetToolTip` напрямую:

```csharp
GlassSettingsToolTipService.SetToolTip(button, GlassSettingsResources.Get("Main_BackToolTip"));
```

Строка оборачивается в `ToolTip { Content = text }` с хуками. Эквивалент XAML из документации Microsoft:

```xml
<Button Content="…" ToolTipService.ToolTip="Simple ToolTip" />
```

— в коде проекта для acrylic нужен сервис (или явный `ToolTip` через сервис).

---

## Как работает acrylic

1. **Стиль** — `BasedOn="{StaticResource DefaultToolTipStyle}"`, свой `ControlTemplate` без fade-анимации (фон и текст появляются сразу, без «прорисовки» текста).
2. **`Loaded`** — найти родительский `Popup` (`GetParent` или `GetOpenPopupsForXamlRoot`), `ShouldConstrainToRootBounds = false`, `SystemBackdrop = DesktopAcrylicBackdrop`, фон tooltip → `Transparent`.
3. **`Opened`** — fallback, если popup ещё не был доступен на `Loaded`.
4. **`Closed`** — восстановить `AcrylicInAppFillColorDefaultBrush` (защита от «залипшего» прозрачного фона при переиспользовании экземпляра).

Не кэшировать флаг «backdrop уже применён» на `ToolTip` между показами — каждый раз новый `Popup`.

---

## Анти-паттерны

- Implicit `Style TargetType="ToolTip"` **без** `DefaultToolTipStyle` — пустой/дефолтный вид.
- `Application.RequestedTheme` в runtime при смене `.theme` извне — см. `WindowThemeBootstrap` (только `RootGrid.RequestedTheme` после старта).
- Polling по `PointerMoved` + `GetOpenPopupsForXamlRoot` — задержка 1–2 с до backdrop.
- `Background = Transparent` до создания `SystemBackdrop` — случайный полностью прозрачный tooltip.

---

## Связанные темы

- Единый фон окна (Mica, прозрачный shell): override `NavigationView` в [`GlassSettingsNavigationChrome.xaml`](../../GlassSettings/Themes/GlassSettingsNavigationChrome.xaml).
- Fade при прокрутке контента (OpacityMask): [`SettingsPageShell`](../../GlassSettings/Controls/SettingsPageShell.xaml), [`SettingsScrollEdgeFade.cs`](../../GlassSettings/Core/Layout/SettingsScrollEdgeFade.cs).
