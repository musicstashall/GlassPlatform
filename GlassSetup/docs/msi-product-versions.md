# Версии продуктов: MSI отдельно от версии контроллера Setup

## Собственная версия Setup

Самообновление Setup использует числовой **FileVersion** EXE из `VS_FIXEDFILEINFO`.
`setup` и `setupPackage.version` каталога относятся именно к нему, а не к
ProductVersion комплекта или `products[].version`. После скачивания FileVersion
EXE сверяется с каталогом; выбор immutable controller и запрет его downgrade
используют тот же FileVersion. Подпись, ожидаемый издатель, hash и протокол
startup ACK проверяются независимо от версии.

`New-ReleaseCatalog.ps1 -IncludeSetup` читает FileMajorPart/FileMinorPart/
FileBuildPart/FilePrivatePart из готового EXE. При публикации изменённого Setup
его FileVersion должен увеличиться. Старый `version.json` с ProductVersion в
setup-полях нужно пересоздать, не подменять версию без пересчёта SHA-256/размера.
Fallback `glass` → setup удалён: версия платформы не версия controller.
Старые выпущенные EXE продолжат выполнять свой прежний код сравнения; для
перехода может потребоваться прямой запуск нового EXE из релиза.

ProductVersion Setup остаётся для существующих bundle/preflight display и EULA
identity; этим изменением их семантика не меняется. Версии MSI независимы.

## Версия GlassSettings

Приложение и оба MSI Settings используют общий источник комплекта:
`GlassProductVersion` и номер сессии. Отдельного `GlassSettingsVersion` нет.
ProductVersion EXE — `GlassBundleVersion`. FileVersion сохраняет per-project
build counter с префиксом `GlassProductVersion`. Native не является источником
версии приложения.

`ProductVersions.props` оставляет версии Settings пустыми, поэтому MSI получает
ту же формулу, что и платформа: базовая тройка плюс номер комплекта. Явная
подмена `GlassSettings64MsiVersion` или `GlassSettings32MsiVersion` допустима
только когда она численно совпадает с ProductVersion упаковываемого EXE.
`Assert-SettingsVersion.ps1` сверяет этот EXE и ProductVersion MSI до harvest.
Несовпадение, отсутствующий или устаревший EXE прерывают упаковку. DLL не
обязательна: поддерживается single-file publish. Ни приложение, ни Native не
запускаются. Проверка не заменяет runtime trust/receipt/identity validation.

Каталог релиза читает ProductVersion каждого MSI. Повторная компиляция в той же
сессии комплекта не меняет эту версию. Новый номер комплекта меняет её вместе
с платформой.

## Источник истины

- **Установлено:** `MsiGetProductInfoW(ProductCode, INSTALLPROPERTY_VERSIONSTRING)`
  конкретного продукта/архитектуры. Не `GlassCore.dll`, не `OpenGlass.dll`,
  не версия EXE Setup и не общий `GLASSBUNDLEVERSION`.
- **Доступный локальный пакет:** `ProductVersion` таблицы `Property` MSI,
  открытого через `MsiOpenDatabaseW(..., MSIDBOPEN_READONLY)`. Discovery проверяет
  ProductCode, UpgradeCode, архитектуру, отсутствие reparse/hardlinks и доверие
  (MSI Authenticode с закреплённым издателем Setup либо release hash).
  Файл удерживается от записи/удаления на время обследования.
- **Предложение релиза:** версия отдельного asset из каталога, который генерируется
  `New-ReleaseCatalog.ps1` из самих MSI. Это метаданные предложения, **не** доказательство
  проверки ещё не скачанных байтов. Перед исполнением проверяются скачанный файл,
  identity/architecture/version и hash. Версия тега релиза не заменяет MSI ProductVersion.
- Неизвестная версия отображается `—`. Не подставляем общую версию/ноль и не считаем
  ошибку чтения признаком более старого продукта. У оригинального SUT установленная
  версия берётся из единственной регистрации vendor MSI. Для доступного SUT
  первична фактическая `ProductVersion` проверенного соседнего MSI. Отдельно
  показывается GitHub release tag; без локального MSI числовой tag служит
  предложением с явной подписью «релиз», а не проверенной MSI ProductVersion.
  Нечисловой tag не превращается в числовую версию продукта.

`Describe` и `Prepare` используют общий read-only discovery и одинаковый выбор
кандидатов. Checking обследует также отсутствующие продукты, но не создаёт workspace,
не копирует/скачивает MSI, не публикует cache и ничего не устанавливает. Загрузка
каталога метаданных остаётся. `Prepare` повторно проверяет приватную рабочую копию
после явного Apply; результаты Checking не являются полномочием на исполнение.

Сравнение числовое, не строковое: `1.10.0 > 1.9.0`; `1.2.3 == 1.2.3.0`.
Поддерживаются версии с major=0. Неизвестные/некорректные значения не участвуют
в сравнении как нули. Execute plan по-прежнему повторно запрещает downgrade
каждого изменяемого продукта относительно его проверенного установленного MSI.
Глобальное сравнение Setup EXE с максимумом версий DLL платформы удалено из preflight:
старый контроллер не означает, что выбранный MSI старее другого, оставляемого продукта.
Проверка разрешённых корней и проверки совместимости Native остаются.

В Installed показывается установленный MSI, в Available — выбранный доступный MSI
(для обновления возможна пара `текущая → новая`). Повреждение остаётся красным,
даже если одновременно найдено обновление. Выбор Apply остаётся явным.

## Как задавать разные версии MSI при сборке

До этого все пять наших MSI получали одну базовую тройку и номер комплекта.
Если фактические ProductVersion одинаковы, одинаковые числа в UI корректны;
придумывать разные версии по имени продукта нельзя.

`GlassSetup/msi/ProductVersions.props` добавлен в проекты MSI и позволяет задать
полную числовую ProductVersion отдельно для каждого пакета:

| MSI | Свойство |
|---|---|
| Glass-x64.msi | `GlassPlatform64MsiVersion` |
| Glass-x86.msi | `GlassPlatform32MsiVersion` |
| GlassSettings-x64.msi | `GlassSettings64MsiVersion` |
| GlassSettings-x86.msi | `GlassSettings32MsiVersion` |
| OpenGlass-x64.msi | `GlassOpenGlassMsiVersion` |

Например, в этом props можно установить
`<GlassSettings64MsiVersion>2.1.0</GlassSettings64MsiVersion>`.
Также допускаются одноимённые глобальные свойства MSBuild. Пустое значение сохраняет
прежнюю bundle base/build политику; существующим выпускам не присваиваются новые
произвольные версии автоматически. `GLASSBUNDLEVERSION` и PE версии не подменяются.
После изменения версии нужен новый MSI, подпись и обновление каталога релиза.

**Ограничение Windows Installer:** собственные upgrade-таблицы MSI учитывают
первые три поля ProductVersion, четвёртое игнорируют. Наш контроллер сравнивает
также четвёртое поле ради совместимости с уже выпущенными bundle-build пакетами.
Для независимого выпуска, который должен различаться средствами самого MSI,
следует повышать одну из первых трёх частей, а не полагаться только на четвёртую.

## Проверка

Portable API seams/реальные production функции и production DOM проверяют:
пять разных версий, legacy/неизвестную регистрацию, ambiguous SUT, числовое
сравнение, отсутствие подстановки PE версии, локальное discovery без package
downloads и без workspace, Available vs Installed. Это не MSVC/WiX сборка,
не Windows Installer и не реальное исполнение WebView2. Windows acceptance:
комплект с пятью разными ProductVersion, offline local packages, смешанная
установка с оставляемым более новым продуктом, downgrade выбранного MSI,
исправный/повреждённый продукт с более новым MSI, оригинальный SUT.

## SecureUxTheme: локальный пакет и GitHub release

`GlassSecureUxPackage::Describe` сначала ищет официальный native MSI по offline
hash v4, затем читает latest release metadata. Если локального v4 нет, допускается
локальный кандидат с hash/size из проверенных метаданных release. Из MSI читаются
identity, architecture и числовая ProductVersion (3–4 части). Локальный файл
удерживается от записи/удаления во время hash/metadata проверки. Никаких MSI
скачиваний, рабочих копий, публикации cache или установок на Checking нет.
Метаданные GitHub запрашиваются и при найденном локальном пакете — для отдельного
показа релиза; недоступность сети не отменяет проверенное offline-предложение.

Порядок: `packages/<canonical native name>` рядом с EXE, рядом с EXE без `packages`,
затем canonical name в защищённом maintenance cache `Current`. Для x64 это
`SecureUxTheme_x64.msi`;
общий alias `SecureUxTheme.msi` не добавлен. `Prepare` использует тот же выбор,
создаёт защищённый snapshot и повторно проверяет его перед исполнением.

HTML-карточка показывает local MSI и release независимо, в том числе при
запланированной установке. Нативный fallback для отсутствующего SUT подписывает
источник версии `MSI` или «релиз»; для установленного показывает версию регистрации,
а не версию доступного предложения. Его Windows/DPI-визуальная проверка ещё нужна.

Проверки: `secure_ux_offer_mock.py` исполняет реальные Describe/discovery/Prepare
с имитацией Windows/hash/MSI API; `run_secure_ux_release_tests.py` проверяет parser
и release version; production DOM и Chromium проверяют приоритет MSI и подписи.
Это не Windows integration test и не проверка реального Authenticode/MSI API.
