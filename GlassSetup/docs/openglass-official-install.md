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

### Post-check после успешного запуска OG

Пользователь сообщил, что OG установлен и запущен, но показана ошибка.
В GlassSetup-27.log (session 20261112_090740) выбран mixed-apply dispatcher rev2;
все четыре Platform/Settings MSI вернули 0 (подтверждено отдельными verbose logs),
Glass committed и запущен. Ошибка пришла из финального OG post-check. Старый
журнал не записывает raw Inno exit, найденный key/path/version и причину mismatch,
поэтому точную ветку отказа ретроспективно установить нельзя. Факт запуска OG
сам по себе не используется как разрешение игнорировать ошибку post-check.

При сверке Scripts/OpenGlass.iss для v3.0.2.3749 обнаружена конструкция
`#define MyAppId "{D3D1BC7D-5E24-4B33-9383-7934271A3B05}"` плюс
`AppId={{#MyAppId}}`: после ISPP получается `{{GUID}}`, а Inno literal escaping
оставляет `{GUID}}`. Наш inventory раньше проверял только canonical `{GUID}_is1`.
Теперь read-only Query проверяет ровно две определённые source-derived identity:
canonical и `{GUID}}_is1`; никаких перечислений по DisplayName, изменения
реестра или миграций нет. Две регистрации одновременно/неизвестный доступ —
ошибка, не выбор наугад. Record сохраняет keyPath, Same проверяет и его, чтобы
смена identity между подготовкой и запуском не прошла незамеченной. Settings
DLL discovery использует те же две identity с отказом при конфликте. Это
исправляет конкретный дефект обнаружения; actual key пользователя старый лог
не содержит, и причина именно его отказа без новых диагностических фактов
остаётся предположением.

Версии post-check сравниваются численно (3.0.2 эквивалентна 3.0.2.0, но не
3.0.2.3749), не строковым равенством. Условия raw exit 0/3010, наличия регистрации,
совпадения каталога и версии сохраняются. Добавлена строка `OpenGlass post-check`
с ran/exit/removing/inventoryKnown/installed/pathMatches/versionMatches,
keyPath, ожидаемыми/фактическими каталогом и версией, inventoryError.

Inventory compiled mock расширен exact-key fallback/conflict и numeric versions;
раньше его fake RegOpen проверял только GUID substring, теперь весь key literal.
Windows/Inno acceptance пока не проводился в sandbox. Загруженные пять исходных
логов после разбора удалены; изображение сохранено.

Сборка: GlassUnins Release|x86 → GlassSetup Release|x86 (общий inventory header);
GlassSettings Release|x64/Release|x86 для исправленного discovery. Native/Shared
код и каталог ресурсов не менялись.

### OG Remove + Platform/Settings Apply в Setup

Разобран GlassSetup-28.log, session 20261112_093001, Setup FileVersion 2.0.0.247.
Пользовательский план — rows=0x28: OG Remove плюс Settings x86 Apply. Локальный
Settings x86 MSI подготовлен успешно; inventory обнаружил отличающийся Native
x86 при той же MSI версии. Ни MSI mutation, ни Inno uninstall не начинались:
dispatcher отправил этот mixed remove в прежний standalone-only gate.
Это ограничение реализации, а не отказ Windows или самого uninstaller.

Теперь MixedOpenGlassRemoval допускает ровно OG Remove вместе с Platform и/или
Settings Apply, без других Remove, SUT, cleanup и force-clean. Используется
существующий sequential-commit dispatcher: PrepareOfficialOpenGlass(removing)
без downloads, исключение OG apply/remove intent и procedure bit из managed
worker, восстановление исходных intent, затем официальный Commit(removing).
При неуспехе/отмене managed части OG не удаляется. После Glass commit ошибка
uninstaller означает явный частичный результат: изменения Glass сохранены,
удаление OG не подтверждено; общий rollback не заявляется. Конфигурация OG
сохраняется, publisher/path checks не изменены. Совместное удаление OG и других
продуктов, а также mixed GlassUnins Remove пока не реализованы.

Compiled mocks: exact user policy rows=0x28; настоящий dispatcher для mixed
removal success, managed failure и vendor partial failure, исключение Remove
bit из managed worker и восстановление исходного плана. Также PASS existing
standalone/uninstaller/GlassUnins/cancellation/fixed-path/source checks.
MSVC/реальное удаление Inno в sandbox не запускались. Исходное log-вложение
после анализа удалено; image-27.png сохранено.
Сборка: GlassUnins Release|x86 → GlassSetup Release|x86, поскольку общий
GlassProducts.h входит в оба проекта. GlassShared не менялся.

### Resources integration: catalog/acquisition foundation (not yet a UI product)

Подтверждено: отсутствие карточки OpenGlass Resources вызвано тем, что planner,
inspection и UI всё ещё содержат прежние шесть продуктов. Наличие wixproj само
по себе карточку не создаёт. Полная интеграция пока **не завершена**, эта правка
не обещает появления активной карточки и не запускает MSI автоматически.

Подключён самостоятельный catalog ID `openglassResources`, файл строго
`OpenGlassResources-x64.msi`. Acquisition проверяет отдельные ProductCode
`{6D6046DB-6B28-4F2C-8A7D-EB6CF13CBBD9}`, UpgradeCode
`{A5571574-00E7-43AD-81DF-B65D1AE07699}`, MSI machine x64, числовую версию,
существующие publisher/hash checks без ослабления. Describe и Prepare отказывают
на native x86; пакет предназначен для AMD64, не для x86 Windows.
Prepared slot 6 выделен отдельно: slot 5 уже занят SecureUxTheme, хотя в MSI
catalog его нет. Старые catalog индексы сохранены. Не использовать индекс нового
MSI как индекс download job до подключения UI/worker.

Публикация независимого дополнения после его сборки:
`New-ReleaseCatalog.ps1 -ReleaseTag <tag> -BaseManifest <version.json> -Products openglassResources`
(из каталога скриптов, либо полный путь к скрипту; PackageDirectory при нужде).
Для первого каталога прежние четыре продукта и IncludeSetup остаются обязательны;
Resources добавляется в Products явно, чтобы отсутствие ещё не собранного
дополнения не ломало существующий выпуск. Генератор сверяет оба GUID и x64,
не выдумывает payload FileVersion для графических файлов.

Осталось: седьмая строка installed inventory и RU/EN/native UI, IPC/download
job, selection/plan dependency, MSI apply/remove/rollback/cache и сценарий
upstream OG commit → Resources commit с честным partial-result. До этого
добавлять фиктивную активную карточку запрещено. На x86 её не предлагать.
Прежний пункт о незавершённой полной Resources integration остаётся актуален.

Проверено: compiled catalog/source-selection tests (включая Resources-only,
старый catalog без дополнения, неверное имя, duplicate ID и prepared slot),
Resources build/source contract, catalog handoff и verify_resources PASS.
Windows WiX/MSVC/реальная установка не проверялись.
Сборка текущих C++ изменений: GlassUnins Release|x86 → GlassSetup Release|x86;
пакет отдельно OpenGlassResources-x64 Release|x64 с непустым graphics source.

### Resources integration: shared policy and read-only inventory

Следующий реализованный слой (UI и Windows mutation adapters ещё не подключены):
`GlassProductPlan::Product::OpenGlassResources` добавлен **в конец**, индекс 6.
Индексы старых шести продуктов не изменены. Planner запрещает Resources Apply
на x86, а на x64 требует OG Installed в after-state без Blocked OG operation.
Самостоятельный выбор графики не выбирает OG автоматически. OG Remove не
расширяет удаление на графику: её можно оставить, затем отдельно удалить без OG.
Старые preview/worker adapters пока не передают седьмой выбор; никакой новой
mutation только от расширения enum не возникает.

Shared scheduler содержит все семь строк ровно по одному разу: установка OG
раньше Resources; при явном удалении обоих — Resources раньше OG. Это policy
порядок, **не утверждение о готовности official commit-last worker**. Для fresh
upstream OG → Resources отдельный адаптер фаз/частичного результата ещё нужен.

`GlassProductInspect::Report::openGlassResources` — независимая регистрация
фиксированного ProductCode, ProductVersion, локальный/серверный offer, attached
media и installed-baseline integrity. Наличие DLL, служб и upstream Resources
не считается наличием этого MSI. На x86 соответствующие inventory/source jobs
пропускаются. Графике не присваивается выдуманный PE payload version и не
назначаются OG service/task checks.

В MSI Launch prerequisite добавлен второй **точный** upstream Inno identity,
уже поддерживаемый Setup; каждый root проверяется своим OpenGlass.dll search.
Два найденных root одновременно не принимаются. Ни один из них не становится
INSTALLDIR: назначение по-прежнему native PF\GlassPlatform\OpenGlass\Resources.
Удаление собственного MSI допускается и без установленного upstream OG.

Проверки: 16384 старых presence/selection cases + 128 dedicated Resources
presence/intent/OS combinations; 16384 семипродуктовых scheduler plans, invalid
plan refusal и остановка на каждой из семи ошибок удаления; compiled catalog,
Resources source/XML contracts, empty-inventory Preview, procedure maps,
official OG engine/mixed worker/Unins mocks и verify_resources PASS.
`setup_execution_plan_mock.py` не скомпилировался: его извлечённый engine harness
не содержит существующие GlassOpenGlassFiles/openGlassPackage_ и методы full
cleanup. Этот тест не считается пройденным. Windows/WiX/MSVC acceptance нет.

Оставшаяся работа: седьмая строка UI/native controls и selection/IPC/downloads,
передача reports в adapters, rollback/cache/root contracts для Graphics MSI,
managed execution и official commit-last fresh OG + Resources. Карточка пока
не появляется; текущая правка не выдаётся за завершение пользовательской задачи.
Сборка: GlassUnins Release|x86 → GlassSetup Release|x86; изменённый пакет —
OpenGlassResources-x64 Release|x64 с graphics source. GlassShared не менялся.

### Resources integration: managed execution and rollback adapters

Добавлен `Selection::openGlassResources`, apply/removal bit 64 и preview row 6.
Флаг по умолчанию выключен; на x86 NormalizeInstall его снимает. Mutation mask
не включает Platform/Settings/OG host bits из-за выбора одной графики.
Собственный Resources MSI вошёл в Prepare, BuildExecutionPlan, RunMsiSequence,
RemoveManagedProduct, related-family recheck и maintenance source publication.
Apply пока требует **уже установленного** официального OG: повторный upstream
inventory плюс безопасный существующий OpenGlass.dll. Он не переписывает путь
upstream OG и не принимает его каталог как INSTALLDIR. Managed remove графики
не требует ни нового media, ни установленного OG.

Новый ResolveOpenGlassResourcesRoot разрешает native PF из системного источника,
проверяет OS и путь/предков. Результат фиксирован:
PF\GlassPlatform\OpenGlass\Resources, независимо от relocated Platform/OG.
Snapshot получает отдельные resourcesDir/backupResources, product mask и family.
Сохраняются как установленный MSI baseline (с проверкой контекста и component
roots), так и существующие файлы, в том числе при отсутствии MSI-регистрации.
Restore/uninstall используют только этот root; Settings-only early rollback
не трогает графику. Старое значение snapshot mask по умолчанию сужено с 0x7F
до 0x3F: новый бит 64 должен запрашиваться явно и не включаться на x86 от
унаследованного default. Default старых шести реальных продуктов не изменён.

Resources MSI получил GLASS_RESTORE_NO_CAB/InstallFiles condition по существующему
протоколу: после возврата файлов из snapshot допускается восстановление
регистрации по cached MSI без встроенного cabinet и без присутствующего OG.
ARP дополнения остаётся штатным MSI: ему не подставляется несуществующий
GlassUnins в каталоге Resources. Full-clean preflight учитывает и его регистрацию;
Resources Apply/Remove вместе с forceClean отклоняется до worker mutations.

**Незавершено:** карточка/нативный control, IPC/download job и forwarding новых
installed facts из UI. Fresh OG + Resources и остальные combined official lanes
с Resources Apply пока явно отклоняются: протокол OG commit → Resources commit
ещё не реализован. Эти правки не означают готовый пользовательский сценарий.
Существующие OG lanes не могут молча проигнорировать новый выбранный флаг.

Проверки PASS: actual BuildExecutionPlan (исправлены границы extraction и
недостающие старые stubs; прежний compile blocker устранён), Resources install,
update/repair/downgrade, отсутствующий/неизвестный upstream, отсутствующий/linked
DLL, неверный media, x86 refusal и removal без OG; 64 snapshot row unions;
1458 procedure maps; actual rollback с отдельной graphics copy и запретом её
Settings-only restore; fixed MSI path/context/rollback guards, fixed native roots,
product policy/scheduler/catalog suites, OG engine/mixed worker/Unins mocks,
empty-inventory preview, cancel-decision и verify_resources.
Ни один mock не является Windows MSI/WiX/MSVC acceptance.

Сборка: GlassUnins Release|x86 → GlassSetup Release|x86; Resources MSI пересобрать
OpenGlassResources-x64 Release|x64. GlassShared не менялся.
