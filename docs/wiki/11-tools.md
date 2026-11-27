# 11 · GlassShellExt, LogViewer и GlassSettings

Помимо ядра инжекции в решение входят три пользовательских инструмента: интеграция с
меню проводника Win11, диагностика в реальном времени и безопасное управление
настройками.

**`GlassShellExt` & Sparse MSIX.** Реализует `IExplorerCommand` для контекстного меню
Windows 11. Регистрируется классическим `regsvr32` (`DllRegisterServer` /
`DllUnregisterServer`) либо компактным меню через `GlassShellSparse` MSIX с трамплином
на `explorer.exe`. Ручная запись ключей `shellex\ContextMenuHandlers`,
`CLSID\…\InprocServer32` и `ExplorerCommandHandler` запрещена.

**`LogViewer` (Named Pipe).** Принимает поток структурированных диагностических
сообщений из `GlassHost`, `GlassCompat` и `UICompat` по каналу
`\.\pipe\GlassPlatform.Log`, позволяя фильтровать лог по PID и подсистеме.
Логи платформы — `%LOCALAPPDATA%\GlassPlatform\Logs`, дампы — `Crashes`.

**`GlassSettings` (WinUI 3).** Клиент на .NET 8 / WinUI 3: редактирование всех
параметров, мгновенный `WM_GLASS_SYNC_CONFIG`, флаг `NoBackdrop=1`, импорт
`.glassbackup v2` (49 OpenGlass-ключей) и миграция профилей OpenGlass.

| Модуль | Артефакт | Роль |
|---|---|---|
| `GlassShellExt` | COM DLL + `GlassShellSparse.msix` | контекстное меню проводника |
| `LogViewer` | C# / named pipe UI | журналы в реальном времени |
| `GlassSettings` | `GlassSettings.exe` + `GlassSettings.Native.dll` | настройки, бэкапы, генерация полотен отражения |

Источник: [docs.html · раздел 11](https://glass-platform.ru/docs.html) · ← [10](10-unload) · → [12](12-build-signing)
