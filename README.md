<p align="center">
  <img src="GlassSetup/res/welcome_banner.png" alt="Glass Platform: стеклянные панели, ползунки и панель задач" width="100%">
</p>

# Glass Platform

Glass Platform меняет отрисовку Windows 10 и 11: прозрачность, стекло, общие цвета и мягкий свет в окнах, на панели задач, в меню «Пуск», в центре уведомлений и в контекстных меню.

Хост подключается к графическим процессам и подменяет отрисовку рамок, фонов, кнопок, полос прокрутки, всплывающих окон и текста: кисти, цвета, порядок закраски и эффекты материала. Окна, меню и ярлыки остаются теми же.

Glass Platform is experimental theming for Windows 10 and 11. It hooks GUI processes and replaces how controls and shell surfaces are painted. License: [GNU GPL v3](LICENSE).

**Где читать документацию:** [Wiki репозитория](https://github.com/musicstashall/GlassPlatform/wiki) · [техническая документация на сайте](https://glass-platform.ru/docs.html) · [справочник реестра](https://glass-platform.ru/registry.html).

## Что меняется

- **Окна и диалоги** — рамки и фоны в цвете рабочего стола.
- **Стандартные элементы** — кнопки, списки, комбобоксы, панели инструментов и прочие common controls в одном стеклянном стиле.
- **Панель задач** — полоса и её элементы рисуются по отдельным зонам.
- **Меню «Пуск», поиск и вылетающие панели** — в общей палитре с остальной оболочкой.
- **Центр уведомлений и календарь** — то же оформление.
- **Контекстные меню** — Win32 и часть меню на панели задач.
- **Рабочий стол** — пункт управления платформой в контекстном меню.
- **Проводник** — окна папок в одной палитре с остальной оболочкой.

Параметры, включение и выключение платформы задаются в **GlassSettings**. Те же действия есть в контекстном меню рабочего стола. Справочник ключей — [docs/registry.md](docs/registry.md).

## Прежде чем включать

**Windows 11 26H2 (сборка 26300):** цикл падений Проводника начинается после установки и запуска OpenGlass. Остановка Glass Platform его не прерывает. Автор OpenGlass в [таблице совместимости](https://github.com/ALTaleX531/OpenGlass/wiki/Compatibility-and-DWM-architectures) помечает 26H2 как Stable, но в опубликованном релизе [v3.0.2.3749](https://github.com/ALTaleX531/OpenGlass/releases/tag/v3.0.2.3749) предупреждает о серьёзных проблемах на сборке 26300.9818 и просит использовать сборку GitHub Actions вместо этого установщика. Пока исправление не вошло в релиз, OpenGlass на 26H2 устанавливайте только осознанно. При этом начиная с ноября 2026 установщик и GlassSettings вооружают «сторожа» падений DWM: при двух крахах dwm.exe в течение 30 секунд после старта OpenGlass сторож немедленно останавливает OpenGlass и отключает его автозапуск (вручную вернуть можно в GlassSettings). Карантин снимается автоматически при установке более новой версии OpenGlass или вручную. Сторож снижает ущерб, но не исключает его: второй краш может успеть завершить сеанс. На машинах, где недопустимы сбои оболочки, OpenGlass по-прежнему не устанавливайте и не запускайте.

**Windows 11 26H2 (build 26300):** the Explorer crash loop starts only after OpenGlass is installed and launched, and stopping Glass Platform does not stop it; OpenGlass v3.0.2.3749 warns of severe issues on build 26300.9818 and says to use the GitHub Actions CI build instead of that installer. As of November 2026 the installer and GlassSettings arm a DWM-crash watchdog: on two dwm.exe crashes within 30 seconds of an OpenGlass start it immediately stops OpenGlass and disables its autostart (re-enable manually in GlassSettings). Quarantine clears automatically when a newer OpenGlass version is installed, or manually. The watchdog reduces damage but cannot eliminate it — a second crash may still take the session down. On machines where an Explorer crash is unacceptable, do not install or launch OpenGlass.

Программа **экспериментальная**: инъекция в GUI-процессы, дополнительная нагрузка на CPU и GPU, без гарантии стабильности. После остановки возможны кратковременные артефакты в проводнике или других процессах, пока они не перерисуются. Не используйте на машине, где сбой или перезапуск оболочки недопустимы.

Подробнее о рисках и отказе от гарантии — [LICENSE](LICENSE).

## Запуск

- Windows 10 или 11. Основная сборка и запуск — x64; x86 собирается отдельно.
- Для старта хоста нужны права администратора.
- **GlassSettings** — единый центр: включение и выключение платформы и все параметры. Программа — `GlassSettings.exe`.
- **Контекстное меню рабочего стола** — те же действия с рабочего стола: включить, остановить и открыть GlassSettings. Пункт регистрируется из GlassSettings.

Готового выпуска нет: платформу собирают из исходников. Установщик описан в [GlassSetup/README.md](GlassSetup/README.md); известное падение Explorer при обновлении остаётся открытым дефектом. Переключатель [SecureUxTheme](https://github.com/namazso/SecureUxTheme) в GlassSettings включает и выключает только регистрацию IFEO/COM. Удаление продукта и оставшиеся файлы ThemeUiProxy описаны там же; перевод GlassUnins на оригинальный ProductCode ещё в работе.

## Сборка

Нужен Visual Studio 2022. Заголовки библиотеки [gtl](https://github.com/greg7mdp/gtl) положите в каталог `gtl` в корне решения, рядом с `GlassProject.sln`. Сборка читает оттуда `phmap.hpp` (`gtl\phmap.hpp`). Каталог в репозиторий не входит. Каталог `tools` тоже локальный и на GitHub не публикуется.

```bat
msbuild GlassProject.sln /p:Configuration=Debug /p:Platform=x64
```

Результат: `Build\x64\Debug\`. Если сборка сообщает, что DLL занята, сначала остановите платформу в GlassSettings.

Сборка установщика, порядок Release x64 → x86, откат и журналы — [GlassSetup/README.md](GlassSetup/README.md).

## Подпись

Официальные Release-сборки подписаны сертификатом **`CN=Stanislav K.`**. Сейчас это временный self-signed сертификат. SmartScreen и UAC показывают «Неизвестный издатель»: подпись подтверждает авторство и целостность файла. Для self-signed `Status` будет `UnknownError` с сообщением о недоверенном корне — при совпавшем отпечатке подпись цела.

| | |
|---|---|
| Subject | `CN=Stanislav K.` |
| SHA-1 thumbprint | `A801E5F5F430D6D19164C856F79B8EFD23F83EB8` |
| SHA-256 fingerprint | `1295F4BD2E1E7D710B380ECE573E8C3C021AE1E21C0E77174E5307E68A7DC93D` |
| Публичный сертификат | `Shared/certs/DeveloperCodeSigning.cer` |
| Действителен | 2026-11-01 .. 2031-11-01 |
| Подписаны сборки | начиная с сборок от 2026-11-01 |

- Отпечаток совпал с таблицей — файл авторский, подпись цела.
- `HashMismatch` — файл изменён после подписи.
- `NotSigned` — файл не из авторской сборки.

В двух последних случаях файл запускать не следует. Устанавливать `.cer` в доверенные корневые центры не нужно. Что именно подписано, команда проверки и режимы сборки — [docs/signing.md](docs/signing.md).

## Состав

| Модуль | Роль |
|--------|------|
| GlassCompat.dll | Хост: WinEvent, инжекция, остановка |
| GlassCore.dll | Перехват отрисовки, темы, диспетчер paint |
| WndCompat.dll | Подклассы и окна приложений |
| ExplorerCompat.dll | Проводник |
| TaskbarCompat.dll | Панель задач |
| UICompat.dll | CoreWindow / «Пуск», поиск, NC |
| GlassSettings.exe | Параметры, включение и выключение |
| LogViewer.exe | Журнал для разработки (только в Debug) |
| GlassShellSparse\GlassShellExt.dll | Контекстное меню рабочего стола (вместе с `GlassShellHost.exe` и sparse-пакетом в подкаталоге `GlassShellSparse\`) |
| GlassSetup.exe | Установщик платформы |

## Документация

Две площадки: в репозитории — версионируемый текст и диаграммы, на сайте — тот же материал с интерактивом и поиском.

- Техническая документация, 13 разделов (архитектура 13 модулей, IPC и объекты ядра, выгрузка `ExecuteShutdown`, XAML Islands): [Wiki](https://github.com/musicstashall/GlassPlatform/wiki) — диаграммы Mermaid; интерактивные схемы, фильтры и симуляторы — [docs.html](https://glass-platform.ru/docs.html)
- Справочник реестра: [docs/registry.md](docs/registry.md) на GitHub, с поиском по параметрам — [registry.html](https://glass-platform.ru/registry.html)
- Инструкции по установке и настройке: [help.html](https://glass-platform.ru/help.html)
- Архитектура: [ARCHITECTURE.md](ARCHITECTURE.md)
- Сборка модулей и правила для разработчиков: [AGENTS.md](AGENTS.md), README каталогов `GlassCore/`, `GlassCompat/`, `GlassSettings/` и остальных проектов
- Установщик: [GlassSetup/README.md](GlassSetup/README.md)
- Подпись: [docs/signing.md](docs/signing.md)

## Связанные проекты

Облик частично вдохновлён [TranslucentFlyouts](https://github.com/ALTaleX531/TranslucentFlyouts). Установщик по желанию ставит два сторонних продукта: [OpenGlass](https://github.com/ALTaleX531/OpenGlass) в каталог Glass и [SecureUxTheme](https://github.com/namazso/SecureUxTheme) для системных тем Windows. Glass Platform продолжает BlackWindowManager. Совместная работа не поддерживается: ключ `Software\StanislavKa\BlackWindowManager` не читается и не пишется, значения из него сами не переносятся.
