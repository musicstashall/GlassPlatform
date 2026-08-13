# GlassProject

Модульная платформа глубокой кастомизации интерфейса Windows: прозрачность, «стекло», темы common controls, popup-окна, проводник и панель задач. Настройки совместимы с веткой реестра StanislavKa / Black Window Manager (`HKCU\Software\StanislavKa\...`).

## Возможности

- Кастомизация отрисовки окон и контролов (кнопки, списки, комбобоксы, панели инструментов и др.)
- **LogViewer** — запуск/остановка платформы и просмотр логов
- **GlassSettings** — окно параметров отрисовки (WinUI 3)
- Контекстное меню рабочего стола (Windows 10 legacy / Windows 11 compact)
- Дополнительные модули: проводник (ExplorerCompat), панель задач (TaskbarCompat)

## Требования

- Windows 10 или 11 (x64; x86 — при сборке)
- **Права администратора** для запуска платформы (хост через `Rundll32` и глобальный WinEvent hook)
- Visual Studio 2022 — только для сборки из исходников

## Быстрый старт

### Сборка

```bat
msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64
```

Артефакты: `Build\x64\Debug\`

### Запуск и остановка

- **LogViewer.exe** — кнопки START / STOP
- Или: `Rundll32.exe "путь\GlassCompat.dll",Main /start`
- Остановка: `Main /stop` или `stop.vbs` в каталоге сборки (если платформа запущена)

### Настройки

- **GlassSettings.exe** — параметры отрисовки
- Регистрация пункта меню рабочего стола — из GlassSettings (shell extension / sparse package)

## Состав сборки

| Файл | Назначение |
|------|------------|
| GlassCompat.dll | Хост и инжекция в GUI-процессы |
| GlassCore.dll | Движок перехвата отрисовки |
| WndCompat.dll | Окна и контролы |
| ExplorerCompat.dll | Проводник (XAML), только explorer.exe |
| TaskbarCompat.dll | Панель задач |
| LogViewer.exe | Логи и управление платформой |
| GlassSettings.exe | Настройки |
| GlassShellExt.dll | Контекстное меню рабочего стола |

## Предупреждения

Проект **экспериментальный**: инжекция в GUI-процессы, возможны артефакты после остановки (особенно в explorer). При ошибке сборки «DLL занята» — остановите платформу штатно (`stop.vbs`), **не завершайте процессы принудительно** без необходимости.

## Для разработчиков

- [docs/README.md](docs/README.md) — индекс документации
- [LICENSE](LICENSE) — учебная лицензия (только личная среда разработки)
- [CLAUDE.md](CLAUDE.md), [AGENTS.md](AGENTS.md) — сопровождение кода и ИИ-ассистенты
