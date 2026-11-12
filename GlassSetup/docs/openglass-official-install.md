# OpenGlass: официальный установщик и отдельные Resources

Актуальная политика от 2026-11-04. Отменяет запрет исполнения upstream EXE,
извлечение payload и ограничение управления только bundled-каталогом Glass.

## Требуемый маршрут Setup (ещё НЕ подключён)

1. На Next искать `OpenGlassSetup.Signed.exe` рядом с исходным GlassSetup.exe.
   Локальный источник приоритетен, не требует доступности release API. Не запускать
   файл из пользовательского каталога напрямую elevated: копировать в защищённый
   workspace, закрепить handles, проверить Authenticode и ожидаемого upstream
   издателя, PE/version; не ослаблять существующую проверку личности издателя.
2. При отсутствии допустимого локального EXE получить официальный release asset,
   проверить HTTPS allowlist, размер, SHA256 release metadata и того же издателя.
   Downloads начинаются только на Next, с существующим footer/card progress.
   После проверки staged-копии обязательно удалить только её ADS
   `:Zone.Identifier` и подтвердить отсутствие потока. Исходный adjacent/Downloads
   EXE не изменять. Ошибка удаления или проверки отсутствия останавливает
   подготовку. После закрытия/reopen handle повторно проверить хеш и подпись;
   снятие метки не заменяет trust checks и не гарантирует отсутствие иных
   блокировок SmartScreen/антивируса/политик системы.
3. Обнаруживать зарегистрированную upstream Inno установку независимо от каталога.
   AppId из upstream `Scripts/OpenGlass.iss`:
   `{D3D1BC7D-5E24-4B33-9383-7934271A3B05}`; HKLM 64-bit uninstall key:
   `Software\Microsoft\Windows\CurrentVersion\Uninstall\{D3D1BC7D-5E24-4B33-9383-7934271A3B05}_is1`.
   Читать InstallLocation/DisplayVersion; не считать любой одноимённый EXE доверенным.
4. Сохранять существующий каталог. Для новой установки default:
   `C:\Program Files\GlassPlatform\OpenGlass` (через known folder, не literal drive).
5. Запускать проверенную staged-копию напрямую, без shell:
   `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /DIR="<resolved directory>"`.
   Не создавать/останавливать/удалять службы, задачи или ACL вместо Inno.
6. Не прерывать установившийся Inno процесс через TerminateProcess на Cancel.
   Дождаться результата; корректно обработать exit code и reboot requirement.
   После завершения перечитать upstream registration и реальное состояние.
7. GlassUnins использует штатный upstream uninstaller с проверкой пути/подписи,
   а не file receipt/MSI/собственное удаление OG service/task. Не подменять Inno ARP.
8. Upstream install не является частью собственного файлового журнала Glass.
   Определить явную commit boundary и предупреждение о частичном успехе при
   последующей ошибке. Не обещать восстановление прежней версии без проверенного
   прежнего upstream setup и реально реализованного recovery.

**Сейчас engine/provider всё ещё используют прежний extraction/file route.**
Этот документ не означает, что запуск EXE уже подключён. До завершения перехода
не считать текущую ветку готовой к проверке установки OG по новой политике.

## Реализованная первая часть

- Settings ищет upstream Inno InstallLocation прежде bundled fallbacks и больше
  не скрывает настройки/управление OG только из-за другого каталога. Root notice
  ведёт в настройки Glass независимо от каталога OG. Исполняемые файлы не
  запускаются в рамках обнаружения; существующие native lifecycle checks остаются.
- Добавлен проект `msi/OpenGlassResources-x64.wixproj`, отдельные ProductCode и
  UpgradeCode, ресурсный payload, зависимость от существующей OpenGlass.dll.
  Удаление MSI возможно после удаления OG; установка/repair требуют OG.
- Graphics input по умолчанию:
  `Build\x64\Release\OpenGlass\Resources`; можно задать `GlassOgResourcesSource`.
  Build отказывает при отсутствии/пустом источнике, reparse points и расширениях
  вне whitelist graphics/data. DLL/EXE/service/uninstaller не включаются.
- Дополнения ставятся в `[ProgramFiles64Folder]GlassPlatform\OpenGlass\Resources`,
  независимо от каталога upstream OG. Inno InstallLocation используется только
  для проверки зависимости (наличия OpenGlass.dll), не как destination MSI.
- Проект виден в solution, но не добавлен в автоматическую Build.0: исходные
  графические файлы не хранятся в этом репозитории, пустой пакет недопустим.

## Ещё не сделано

- Официальные acquisition/install/uninstall/commit boundary в Setup и GlassUnins.
- Общая native inventory Inno и пути во всех потребителях; policy очистки/retirement.
- Resources как отдельный selectable продукт в release catalog, UI, planner,
  maintenance cache, uninstall ordering и rollback. Один WiX-проект не закрывает N7.
- WiX/MSVC/WinUI сборка и Windows/VM acceptance; Linux source contracts не заменяют её.

## Сборка первой части

- Settings: актуальные GlassShared → GlassSettings.Native → GlassSettings,
  Release|x64 и отдельно Release|x86 (неизменённые зависимости можно не пересобирать).
- Resources: подготовить graphics source, затем из корня solution
  `msbuild GlassSetup\msi\OpenGlassResources-x64.wixproj /p:Configuration=Release /p:Platform=x64`.
  Существующая политика подписи MSI действует; сертификаты автоматически не ставятся.
- Setup/GlassUnins в этой части не менялись. Для новой upstream установки их
  пересборка сама по себе новый маршрут не включит.

### Подготовлены API, ещё не подключённые к worker

`GlassOpenGlassPackage::PrepareInstaller[ForCurrentSystem]` реализует приоритет
проверенного adjacent EXE и fallback к release, без извлечения payload.
`RunInstaller` реализует прямой silent запуск/ожидание и возвращает raw exit code;
с `/RESTARTEXITCODE=3010` вызывающая сторона обязана отдельно учитывать reboot.
Обе функции проверяют защищённую staged-копию, а не запускают пользовательский
файл прямо из «Загрузок». Вызовы этих API из engine ещё не включены: требуется
одновременно исключить его собственные lifecycle/file операции над OG.
Старый OpenGlass-x64 MSI-проект и его сборочные/публикационные ветки удалены.

### Read-only Inno inventory (подготовительный шаг)

Добавлен `GlassOpenGlassInstall.h`: чтение только точного HKLM64 Inno AppId,
`InstallLocation`, числового `DisplayVersion` и штатного `UninstallString`.
Неожиданная команда, повреждённая строка или недоступный ключ — ошибка, а не
«продукт отсутствует». `HasProduct` при ошибке консервативно сохраняет ссылку
на продукт; `Same` позволяет сверять ранее прочитанное состояние.

Это **не разрешение на запуск**: каталог, подпись и файлы uninstaller ещё должны
проверяться отдельно непосредственно перед исполнением. API пока не вызывается
ни engine, ни GlassUnins. Намеренно не переключено одно только обнаружение пути:
старые file/service операции не должны получить доступ к upstream-каталогу до
согласованного подключения нового lifecycle/commit boundary.

`openglass_install_inventory_mock.py` компилирует настоящий header с fake Win32:
произвольный каталог, отсутствие/отказ доступа, тип/терминатор/embedded NUL,
неожиданная команда и версия, закрытие ключа. Это не Windows acceptance.

### Подключён изолированный маршрут Setup (промежуточный этап)

Выбранная строка OpenGlass теперь перехватывается в начале worker **до** старого
CapturePriorHostState/snapshot/stop/MSI/file/service pipeline. Разрешена только
отдельная установка/обновление на x64. Смешанный план, Remove, force-clean,
cleanup и миграция retired MSI/receipt отклоняются до системных изменений.
Это временное ограничение, а не завершённая интеграция всех продуктов.

Маршрут читает официальный Inno inventory, сохраняет его каталог; при отсутствии
регистрации использует native Program Files\GlassPlatform\OpenGlass и не
усыновляет уже существующий каталог. Зарегистрированный каталог должен пройти
проверку защищённости. Подготовка использует official adjacent-first API; Next
preparation также больше не извлекает payload. MOTW снимается со staged-копии.

После подготовки и проверки downgrade worker атомарно закрывает отмену через
существующий decision gate/status lock, повторно сверяет регистрацию и вызывает
официальный RunInstaller. Никакие собственные службы/задачи/ARP/receipt не
создаются; запуском компонентов распоряжается upstream. При 0/3010 дополнительно
проверяются Inno каталог и версия; 3010 даёт предупреждение о перезагрузке.
После попытки запуска ошибка явно сообщает о возможном изменении upstream и
**не утверждает**, что Inno был откачен. Собственный rollback не запускается.

GlassUnins, общий inventory/UI установленного продукта, mixed-product commit,
полная Resources integration ещё не готовы. Миграция старых тестовых установок не планируется: их нужно предварительно удалить прежним деинсталлятором.
Старый общий код пока сохранён, но выбранная строка OG до него не доходит.
`openglass_engine_lane_mock.py` проверяет pure selection policy и source-order
контракты; runner/ADS/inventory имеют отдельные compiled mocks. Windows VM
acceptance (Inno/SCM/reboot/cancel/partial failure) и MSVC сборка ещё нужны.

Сборка для проверки: **GlassSetup Release|x86**; GlassShared/GlassUnins не менялись.

### Отдельное штатное удаление через Setup

Ограничение выше уточнено: standalone Remove OpenGlass теперь поддерживается
новым маршрутом Setup. Нужна ровно одна Remove-строка OpenGlass, без Apply других
продуктов, force-clean или cleanupUserData. Смешанные планы по-прежнему отклоняются до изменений. Миграция старых тестовых установок намеренно не реализуется. GlassUnins пока не подключён к новому runner.

`RunUninstaller` повторно проверяет точную Inno-регистрацию, защищённость каталога,
EXE и `unins000.dat`, обычные single-link файлы и подпись ожидаемого upstream
издателя. Никакого shell/TerminateProcess и изменения ADS оригинала. Pins
освобождаются перед запуском: Inno должен иметь возможность удалить свои файлы.
Команда: `/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /RESTARTEXITCODE=3010 /NODELETECONFIG`.
Именно `/NODELETECONFIG` сохраняет текущую пользовательскую и машинную
конфигурацию: в текущем upstream .iss её удаление включено по умолчанию.
`/DELETEPRESETS` не передаётся. Пользовательские данные не удаляются без opt-in.

Удаление не готовит/скачивает installer. После raw 0/3010 Setup дополнительно
проверяет отсутствие Inno-регистрации; ошибка/неизвестное состояние не считается
успехом и не порождает заявления об upstream rollback. Службы, задачи и файлы
удаляет только Inno. Перезагрузка обрабатывается как предупреждение.

Read-only discovery Setup теперь учитывает официальный Inno inventory и читает
DLL FileVersion из зарегистрированного произвольного каталога; отсутствие нашего
bundled OpenGlass-каталога не объявляет официальную установку повреждённой.
Это не hash-проверка payload и не право владения его службами. Lifetime controller
также сохраняет ссылку на официальный продукт, включая неизвестный результат
чтения регистрации, вместо преждевременного retirement maintenance-cache.

**Ограничение доверия:** реальная подпись генерируемого upstream `unins000.exe`
в этой среде не проверялась. Если он unsigned/не проходит существующую проверку
ожидаемого publisher, runner откажет до запуска. Наличие подписанного setup само
по себе не доказывает подпись uninstaller; trust policy не ослаблена.

Проверки: compiled mock настоящего uninstall runner (trust/publisher/registry,
EXE+data pins, command, сохранение config, raw exit codes, handles/errors), lane
policy/source guards, inventory, installer/ADS и lifetime mocks. Это не MSVC и не
Windows uninstall acceptance. Для проверки на Windows нужна VM со snapshot.

Сборка после этого шага: **GlassUnins Release|x86 → GlassSetup Release|x86**
(общий inline lifetime header изменён; нового линковочного модуля нет).
GlassShared пересобирать не требуется. При проверке удаления используйте Setup,
а не ещё не подключённый к official runner GlassUnins.


### GlassUnins подключён к official removal

Standalone OpenGlass Remove теперь использует тот же `RunUninstaller` в Setup и
GlassUnins. Runner вынесен в отдельный `GlassOpenGlassUninstall.cpp`, а прежние
pin/Authenticode/exact publisher/hash helpers без ослабления политики вынесены
в `GlassOpenGlassTrust.h`. GlassUnins не линкуется с acquisition/download/extract
модулями; ему добавлены только uninstall translation unit и bcrypt.lib для
существующего signer hash. Оба vcxproj компилируют общий runner ровно один раз.

GlassUnins read-only inventory и версия карточки теперь учитывают точную Inno
регистрацию независимо от bundled дерева. Выбранная строка OpenGlass
перехватывается до старых root/MSI/file/service probes. Отдельный маршрут
повторно сверяет регистрацию с показанной пользователю, атомарно закрывает
отмену, вызывает runner с сохранением конфигурации и подтверждает отсутствие
регистрации. Ошибка, неизвестное состояние и retained registration — не успех;
автоматический откат upstream не заявляется. 3010 сохраняет reboot pin.

Mixed-product Remove и cleanupUserData пока отклоняются до изменений.
Официальный продукт также учитывается как ссылка, запрещающая глобальную
очистку при его сохранении. Последующие этапы: согласованный mixed-product
commit и полноценная интеграция Resources; **миграции старых установок не нужны
и не планируются**, поскольку публичного выпуска ещё не было. Старую тестовую
установку нужно удалить её прежним деинсталлятором перед проверкой нового пути.
Старые строки документа о «миграции ещё не готова» не являются планом её разработки.

`openglass_unins_lane_mock.py` компилирует настоящий метод GlassUnins с fake
Win32/vendor API: отдельный выбор, ранний отказ mixed/cleanup/retired identity,
конкурирующая отмена, изменившаяся регистрация, raw ошибки, post-check и reboot.
Отдельный compiled runner mock продолжает проверять command/trust/pins/handles.
Подпись реального upstream uninstaller и Windows runtime всё ещё требуют VM
acceptance; unsigned uninstaller не запускается, доверие не ослаблено.

Сборка: **GlassUnins Release|x86 → GlassSetup Release|x86**. Shared/Settings и
полный solution rebuild не требуются. Native fallback и HTML используют один
новый RunOperation dispatch, отдельного сценария удаления в UI не добавлено.

### Mixed apply: Platform/Settings + OpenGlass

Setup теперь допускает общий Apply-план OpenGlass с Platform и/или Settings.
Это **последовательные commits**, не атомарная транзакция двух установщиков:

1. До managed Glass изменений разрешаются Inno inventory/каталог, проверяется
   старая тестовая identity, подготавливается проверенный official EXE, запрещается
   downgrade. Ошибка подготовки не запускает managed worker.
2. На время managed worker строка OpenGlass исключается из selection и procedure
   mask. Следовательно, она не входит в Glass snapshot, stop/register/launch,
   managed removal и file-transaction pipeline. Исходные snapshot/rollback
   гарантии остальных выбранных Glass-продуктов сохраняются.
3. Если managed worker неуспешен/отменён, Inno не запускается. Если он успешен,
   Glass уже committed; отмена остаётся закрытой. Повторно сверяется официальный
   OpenGlass inventory и запускается official EXE.
4. Ошибка финального Inno шага даёт явный **частичный результат**: Glass сохранён,
   OpenGlass не подтверждён, общий rollback не выполнялся. Это относится и к
   изменившейся перед Inno регистрации. Заявлять восстановление всех продуктов
   по Glass snapshot после отдельного vendor commit нельзя.

Worker split: внешний `WorkerThread` — dispatcher и единственное место публикации
finished/финальной очистки staging; внутренний `RunGlassWorker` выполняет прежний
managed pipeline. Staged EXE живёт до завершения последнего Inno шага, managed
worker не выставляет finished и не уничтожает его преждевременно. Prepare/Commit
OpenGlass используют одни методы для standalone и mixed сценариев.

Сочетание OpenGlass с SecureUxTheme, mixed Remove, force-clean и user-data cleanup
пока отклоняется до изменений. Смешанное удаление в GlassUnins также пока не
включено. Миграции старых тестовых установок намеренно не реализуются.

`openglass_mixed_worker_mock.py` компилирует настоящий dispatcher: prepare failure,
managed failure/cancel-equivalent, последовательность commits, OG row/mask
exclusion, сохранение staging, partial failure и deferred finished. Остальные
compiled mocks/source guards покрывают предыдущие операции. Это не Windows
acceptance: на VM ещё нужны реальные MSI/Inno, cancellation, partial failures,
reboot, cache и сохранность upstream service/config.

Сборка этого шага: **GlassSetup Release|x86**. GlassUnins, GlassShared и их
линковочные зависимости в этом шаге не менялись; full solution rebuild не нужен.


### Исправлен контракт каталога графических дополнений

Resources принадлежат GlassPlatform, а не внешней установке OpenGlass. Default
MSI destination: native Program Files\GlassPlatform\OpenGlass\Resources.
Например, при OG в D:\Apps\OpenGlass дополнения остаются в
C:\Program Files\GlassPlatform\OpenGlass\Resources (если native Program Files
этой Windows — C:\Program Files). Внешние Resources не заполняются этим MSI.

`UPSTREAMOPENGLASSROOT` — отдельное read-only discovery свойство зависимости,
не directory payload. Каталог MSI строится независимо от него от
`ProgramFiles64Folder`. `PlatformPaths.OpenGlassResourcesRoot` и
`OpenGlassResourcesDirectory` используют тот же native Program Files из HKLM64,
включая x86 Settings на x64 Windows. ThemeImageCatalog ищет встроенные дополнения
только в этом корне; внешние DLL/Host/GUI продолжают разрешаться по официальному
Inno InstallLocation. Указанные пользователем custom asset paths не меняются.
При недоступном системном пути Settings не подставляет внешний OG/AppBase.

Проверка `openglass_resources_contract.py`: XML дерево MSI/независимость
upstream search от destination и source wiring Settings/ThemeImageCatalog.
Это не WiX/MSVC/.NET runtime acceptance. Полное подключение Resources к
catalog/UI/planner всё ещё впереди. Миграций прежних тестовых установок нет.

Сборка: отдельно OpenGlassResources-x64.wixproj Release|x64 (нужен graphics
source), затем GlassSettings Release|x64 и Release|x86 для нужных пакетов.
Изменений native/shared/GlassUnins/GlassSetup C++ в этом исправлении нет.

### Диагностика старого запрета смешанного плана

Разобран пользовательский GlassSetup-26.log (session 20261112_085450): четыре
локальных Platform/Settings MSI успешно проверены Authenticode и подготовлены
с source=2, network=0, ok=1. Отсутствие пробного подпути packages не мешало
успешному adjacent fallback. Для OG использован release EXE (4049232 bytes,
network=1); SecureUxTheme не выбран. До MSI execution worker отказал дословным
ранним текстом «Пока выполняйте установку/обновление OpenGlass отдельно…».
Это старый engine без mixed-apply dispatcher, а не ошибка доступности локальных
MSI. Запущенная копия находилась в защищённом Installer/hash каталоге; журнал
не содержит фактическую FileVersion, поэтому точный исходный build и причину
выбора этой копии по этому журналу установить нельзя. Installer cache не менялся.

Для последующих журналов добавлены Running Setup FileVersion и OpenGlass
`dispatcher revision=2: route=...` с rows/apply/remove/SUT/forceClean. Текущий
неподдерживаемый OG-план возвращает ERROR_NOT_SUPPORTED, а не MSI policy code
ERROR_INSTALL_PACKAGE_REJECTED: наше ограничение плана не должно объясняться
как запрет установки политикой Windows. Trust/ownership отказы не ослаблены.
Исходное загруженное log-вложение удалено после анализа; изображение сохранено.
Сборка диагностического исправления: GlassSetup Release|x86.
