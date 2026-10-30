# GlassSettings (WinUI 3)

Окно настроек Glass на **WinUI 3** + **C# .NET 9** (unpackaged x64).

## Проекты

| Путь | Назначение |
|------|------------|
| `GlassSettings/` | WinUI + Core (реестр, snapshot, placement, live reload) в одном `.csproj` |
| `GlassSettings/Native/` | `GlassSettings.Native.dll`: GlassCore (превью текста, live reload), IFileOpenDialog, shell extension |

**Host GlassCore** (загрузка движка в процессе настроек, без WndCompat): [GlassSettings-host-glass.md](GlassSettings-host-glass.md).

## Сборка

**Только MSBuild** из корня solution (как остальные проекты):

```bat
msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64
```

Артефакты: `Build\x64\Debug\GlassSettings.exe`, `Build\x64\Debug\GlassSettings.Native.dll`, кэш: `Cache\GlassSettings\x64\Debug\`.

Не использовать `dotnet build` для проверки — другие intermediate/output и VS пересобирает solution.

Требуется запуск от администратора (`requireAdministrator`).

## MVP

Обзор, Система, **Настройки Glass** (hub + Root/Popup/Taskbar), DWM, Процессы, **Визуальные стили**; placement `...\GlassSettings\MainWindowPlacement`; `GlassAlphaColorPicker`; host-glass для Win32 picker.

Разработка UI: [GlassSettings-development.md](GlassSettings-development.md). **Crossfade обоев в превью:** [GlassSettings-wallpaper-preview-crossfade.md](GlassSettings-wallpaper-preview-crossfade.md). **Page operation pipeline (detail-транзакции):** [GlassSettings-page-pipeline.md](GlassSettings-page-pipeline.md). **Уведомления и зеркало:** [GlassSettings-notices.md](GlassSettings-notices.md). **Hosted-диалоги и штора:** [GlassSettings-hosted-dialog.md](GlassSettings-hosted-dialog.md). **Host-glass / live reload:** [GlassSettings-host-glass.md](GlassSettings-host-glass.md). **Baseline реестра / gate:** [GlassSettings-customization-tracking.md](GlassSettings-customization-tracking.md). **Визуальные стили:** [GlassSettings-visual-styles.md](GlassSettings-visual-styles.md). **Локализация:** [GlassSettings-localization.md](GlassSettings-localization.md). Типы карточек: [GlassSettings-detail-cards.md](GlassSettings-detail-cards.md).

Подробнее: [assistant/GlassSettings-WinUI-chat-prompt.md](../assistant/GlassSettings-WinUI-chat-prompt.md).
