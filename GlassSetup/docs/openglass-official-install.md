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
   Не создавать/удалять службы, задачи или ACL вместо Inno. Перед удалением
   допускается только проверенная остановка OG по общему протоколу ниже.
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

### Resources integration: Setup card, native control, IPC and downloads

Карточка **OpenGlass Resources** теперь подключена к ComponentsJson на x64,
с каталогом `openglassResources` (MSI catalog index 5), отдельным installed
report, MSI-версиями серверного/локального источников, capability и состоянием.
На x86 hidden=true; нативный флажок также скрыт. Startup/maintenance inventory,
preview, full-clean review и HasSelectedComponent учитывают дополнение.
Apply/Cancel/Remove проходят стандартный component IPC; removal bit 64 не
пересекается с SUT и Settings32. Нет неявного выбора OG или других продуктов.

Поддерживаемый пользовательский сценарий этого этапа — Resources при **уже
установленном официальном OG**, без OG Apply/Remove в том же плане. При отсутствии
зависимости или выборе операции OG карточка сохраняется в каталоге, Apply
недоступен; компактная нейтральная подпись и подробности RU/EN объясняют причину.
Remove установленного/осиротевшего Resources остаётся доступен без OG. Если
пользователь сначала выбрал Resources, затем OG, Next блокируется и показывает
пояснение о раздельных операциях; пользовательский выбор не снимается молча.
Bulk recommendations, если рекомендуют OG, не добавляют конфликтующий Resources
в тот же batch. Fresh OG + Resources за один проход всё ещё не реализован;
это явное ограничение, а не утверждение о полной готовности интеграции.

Download coordinator расширен до семи UI jobs: 0..4 прежние Glass/OG, 5 SUT,
6 Resources. Для job 6 MSI catalog index явно равен 5; prepared hint slot 6,
не SUT slot 5. Resources использует существующий trusted acquisition, private
workspace, progress/cancel/retry и cleanup lifetime. Prepare/download стартует
только после Next, не при появлении/выборе карточки. Источник beside Setup либо
packages/ — `OpenGlassResources-x64.msi`; remote требует опубликованной записи
openglassResources в version.json. Наличие карточки не заменяет наличие пакета.
Серверную запись создаёт ранее подключённый New-ReleaseCatalog.ps1.

Native fallback: отдельный флажок в четвёртом ряду, язык/шрифты/DPI/видимость/
loading/debug locks подключены. При недоступной зависимости в подписи есть
пояснение; выбранный конфликтующий флажок остаётся доступен для отмены.
Реальная проверка компоновки WebView2/GDI на Windows ещё нужна.

PASS: новые compiled actual UI DownloadSelection/HasSelectedComponent/
ComponentsReady adapters (архитектура/зависимость/конфликт/Next wait/orphan remove),
production RU/EN DOM seventh card/icon/refusal/Apply/Remove/Cancel/progress/x86
hiding/recommendations; coordinator с семью параллельными fake providers под
ASan/UBSan, проверка независимых SUT/Resources hints и очистки; existing Next gate,
семипродуктовый download DOM, recommendations и composition motion DOM; product
planner/scheduler/catalog, actual execution plan, OG engine/mixed worker,
empty-inventory Preview, Resources XML/source contract и verify_resources.
Windows/MSVC/WiX/реальные install/remove/rollback здесь не запускались.

Сборка: GlassUnins Release|x86 → GlassSetup Release|x86 (общий HTML/header).
OpenGlassResources-x64 Release|x64 нужен для получения самого MSI; поместить
проверенный подписанный MSI рядом с Setup или опубликовать его с каталогом.
GlassShared не менялся. Единый fresh OG→Resources commit/partial-result протокол
и Resources controls в GlassUnins остаются следующими незавершёнными частями;
штатный ARP uninstaller самого Resources MSI сохранён.

### Completed Setup install/update lane: OG + Resources in one pass; no icons

Этот раздел заменяет прежние промежуточные ограничения «устанавливать отдельно»
для **Apply OG + Apply Resources**. Теперь доступны fresh install и update обоих
за один проход Setup, также с выбранными managed Platform/Settings Apply.
Resources не выбирает OG автоматически: на свежей системе сначала выберите OG,
затем Resources, ставший доступным в том же плане. При уже установленном OG
Resources можно по-прежнему устанавливать отдельно. Recommendations больше не
исключают совместимую пару. x86 exclusion сохраняется. Другие ограничения
(совместные vendor SUT, смешанное удаление, forceClean) не расширены.

Официальная команда OG теперь содержит пользовательские параметры:
`/VERYSILENT /SUPPRESSMSGBOXES /NORESTART /NOICONS /MERGETASKS="!desktopicon"`.
Дополнительно сохранены `/SP- /RESTARTEXITCODE=3010`, явный защищённый staging log
и проверенный `/DIR`. Новая установка по умолчанию использует native
Program Files\GlassPlatform\OpenGlass. Обновление сохраняет подтверждённый
зарегистрированный путь существующего официального OG; миграций/перемещения
внешней установки нет. Setup сам ярлыки upstream не создаёт/не удаляет.

Dispatcher:
1. PrepareOfficialOpenGlass с identity/publisher/path/version checks.
2. Trusted acquire Resources MSI + отдельный snapshot bit64 до **любых**
   изменений. BuildExecutionPlan проверяет media, версию, контекст, installed
   baseline. Только в этом private preflight planned OG считается будущей
   зависимостью; после commit требуется реальный upstream inventory/DLL.
3. Managed Glass phase без OG и Resources rows. При плане только OG+Resources
   пустая managed phase пропускается. Неуспех/отмена не запускают vendor.
4. Официальный OG commit с повторным inventory и проверкой результата.
5. Только после подтверждения OG — OfficialResourcesPhase(false): повторная
   проверка плана/зависимости и MSI Resources в собственном fixed root.

Resources snapshot хранится отдельно от managed rollbackSession, не очищается
managed commit и не пересоздаётся после OG. Scoped swap меняет только временный
graphics adapter context и возвращает selection/plan/mask/snapshot при любом
выходе. При ошибке MSI учтена его возможная частичная регистрация; rollback
удаляет только проверенный Resources MSI и возвращает собственный baseline.
При неуспехе восстановления его workspace сохраняется для диагностики/recovery.
OG и уже committed Glass не откатываются. Итог явно сообщает о частичном
выполнении, даже если отдельный откат Resources удался. Ошибка/неподтверждённый
commit OG не запускает Resources. Финальный 100% vendor status отложен до
завершения Resources. После границы OG commit cancellation закрыт; вводить новый
откат upstream или выполнять второй cancellable общий Glass worker нельзя.

Проверки PASS: exact launcher command/trust/Zone/wait/exit; настоящий dispatcher
(success, отказ каждого prepare, managed failure, OG failure, Resources failure,
план только OG+Resources, восстановление selection и staged lifetime); настоящий
Resources phase с ASan/UBSan (preflight-before-mutation, отдельный baseline,
post-OG recheck, fresh/update MSI failure rollback, recovery retention и exception
scope restoration); actual BuildExecutionPlan проверяет ограниченность planned
prerequisite только preflight; UI/Next capability tests и production RU/EN DOM
проверяют совместимые recommendations. Также регрессии planner/scheduler,
catalog, snapshots/rollback, cancellation, OG Unins, downloads и resources.
Это не Windows/MSVC/WiX/Inno acceptance: в Linux sandbox установка не запускалась.

Сборка: GlassUnins Release|x86 → GlassSetup Release|x86; сам графический пакет —
OpenGlassResources-x64 Release|x64. Подписанный MSI рядом с Setup/в packages либо
запись openglassResources в release catalog обязательны. GlassShared не менялся.

### Resources MSI maintenance after OG removal

The upstream OG prerequisite applies only to a new Resources installation.
The MSI launch condition accepts `Installed`, so ARP Change, repair, reinstall
and removal of an existing Resources product remain available even if OG has
already been removed. This does not reinstall OG or change ownership of its
files/services. Fresh Resources installation still requires a single recognized
upstream registration and its DLL; the rollback restore bypass is unchanged.
Rebuild and publish `OpenGlassResources-x64` (`Release|x64`) to deliver this fix;
already cached MSI packages retain their old launch condition.


### OG presence and pre-uninstall stop (service deletion pending)

Official Inno registration is the OG card presence authority. A service, task,
empty directory or Resources-only directory without that registration is not an
installed/damaged OG and does not offer residual cleanup. Retired MSI and receipt
presence are retained as separate inventory/log facts; no migration is performed.
With registration, the DLL must be a safe regular file with readable version.
Service/task absence does not add an OG integrity error.

`GlassOpenGlassService` probes only SCM status, never starts/creates a service to
try to detect 1072. A confirmed `ERROR_SERVICE_MARKED_FOR_DELETE` is logged as
Windows deletion pending, not product damage. Setup preflight and the installer
execution boundary refuse vendor installation in this state with RU/EN advice to
close foreign service-management handles or restart Windows. A status query that
reports STOPPED cannot prove that Windows has no hidden pending deletion; there
is no guaranteed read-only probe for every such state.

The common official uninstaller runner (Setup standalone/mixed and GlassUnins)
verifies the official registration and trusted signed uninstaller/data first,
then validates service configuration/running image and known task action paths
against the registered OG directory. It requests a bounded stop through the
existing OG service/task stop helper, independently confirms SERVICE_STOPPED (or
service absence), and closes its SCM/service handles before launching Inno.
Timeout, access failure, foreign paths or confirmed 1072 prevent vendor execution.
No service/task deletion, startup-type/ACL change, process kill or automatic reboot
is performed. Only explicit OG removal invokes this stop. Mixed managed actions
still commit first; a later OG preparation/removal failure remains partial
completion, never a global rollback claim. A post-uninstall confirmed 1072 yields
restart-required completion, provided the official registration was removed.

Build: GlassUnins Release|Win32 (solution x86), then GlassSetup Release|Win32
(solution x86). Repackage MSI products that distribute the embedded unins000.exe
when publishing those packages. GlassShared and Resources payload are unchanged.

Tests: `openglass_official_service_mock.py` compiles the actual shared helper;
`openglass_residual_mock.py` checks the actual diagnostic probe/UI classification;
actual official launcher mocks cover stop refusal/no process execution and the
pending-deletion restart result. Mixed worker, GlassUnins lane, Resources phase,
execution-plan, product-plan and resource-source checks pass. These are mocked
Win32/source tests, not MSVC/SCM/Scheduler/Inno acceptance on Windows.
Older unrelated harnesses `task_scheduler_stop_mock.py`, `unins_task_stop_mock.py`
and `setup_real_entry_mock.py` currently fail on missing mock APIs/stale extraction
assumptions; they were not treated as passing acceptance or changed in this fix.

### Existing OG destination on fresh official installation

A missing Inno registration no longer requires the default OG directory to be
absent. A pre-existing safe, administrator-controlled regular directory is allowed,
including one containing Resources or retained configuration. No files are deleted
by this gate; official Inno handles installation. Files alone do not become OG
registration. Reparse points, unsafe paths, untrusted ACLs, a file in place of the
directory, unreadable paths and retired managed test receipts still fail safely.
`openglass_existing_directory_mock.py` compiles this actual RU/EN gate with fake
Win32 APIs. Build GlassSetup Release|Win32 (solution x86); GlassUnins, GlassShared
and the Resources MSI are unchanged by this particular fix.

### Follow-up: Resources ARP Change, idle dependency cards, cached controller

The supplied GlassSetup-29 and five MSI removal logs show Resources, Platform
x86/x64 and Settings x64/x86 removals all returning 0, with successful registration
postchecks. Entries disappearing on reopening Windows Installed apps are consistent
with a stale Settings list, not persistent product registration. Do not delete
additional registry keys to force that list to refresh. Original supplied logs were
removed after analysis; screenshots are retained.

Resources previously had no ARP redirect: the UI-less MSI maintenance action could
show a progress window and exit without opening GlassSetup. RefreshManagedArp now
sets its Change command to the trusted cached GlassSetup --modify --product GUID,
while retaining direct Windows Installer /x removal (not the unfinished Resources
GlassUnins lane). The WindowsInstaller ARP flag is removed just as for other managed
entries; Windows Installer product registration is untouched. New Setup startup
refreshes existing registered Resources entries. No foreign Inno ARP key is edited.

UI preview clears warnings on absent, unselected product rows even when an unrelated
product such as SUT is installed. Actual retained/selected-product warnings and
execution safety gates remain intact. Empty inventory behavior is unchanged.

When a server catalog offers newer Setup than the running EXE, acquisition first
tries the protected cached controller. Only matching server size/SHA256 bytes are
copied from a pinned handle to this operation's workspace; the same Authenticode,
expected-publisher and final FileVersion checks still run. Otherwise network
acquisition remains the fallback. Cached code is never deleted on a version-check
failure: the caller receives only its own staged copy.

Not completed by this follow-up: combined original-SUT plus managed-product removal
(the current explicit restriction can disable Apply for Remove all); official OG
Change integration (upstream owns Inno registration); Resources MSI empty-parent
cleanup for direct uninstall; forced live refresh of the Windows Settings app list.
Do not advertise these as implemented or suggest recursive removal of retained OG
settings. Rebuild GlassUnins Release|Win32 then GlassSetup Release|Win32 for shared
support/UI policy changes; republish embedded uninstallers where applicable.

Checks: setup_download_mock (actual preparation, including zero-network cache reuse
and signature failure), resources_arp_maintenance_mock (actual registry adapter),
empty_inventory_preview_mock (actual preview including SUT-only idle inventory),
controller_handoff_mock, product-plan matrix, resource smoke. No Windows build or
real ARP/Settings UI test was performed in the sandbox.

### Completed follow-up: mixed SUT removal and installed-app notifications

This supersedes the earlier follow-up's separate-only original-SUT removal
restriction in Setup. Original SUT removal may now accompany managed product
removal (Platform, Settings and Resources); SUT installation plus managed removal
remains rejected, as do unsafe/conflicting inventory, profile or helper states.
The official OG dispatcher restrictions are not expanded by this change.

All original-MSI identity, Glass baseline/snapshot, style/profile and managed
removal preflights run first. Hosts are stopped/modules released and any explicitly
selected helper update runs before the private SUT transaction. In mixed removal,
SUT is removed while the trusted installed Native helper is still present, before
RunMsiSequence can remove Settings. The existing loaded-profile/stock-style and
manual-risk rules are unchanged. Cancellation closes atomically at this earlier
vendor boundary. SUT is never executed a second time in the final vendor stage.
If subsequent managed work fails, Glass recovery runs separately and the result
explicitly reports partial completion: SUT was removed and is not restored. No
successful global rollback is claimed. Full user-data cleanup remains opt-in and
only runs after complete success.

The MSI wrapper sends a bounded WM_SETTINGCHANGE notification for the Uninstall
registry section after successful removal; the final ARP refresh sends another
notification at batch completion. This requests live refresh of consumers, without
terminating/reopening SystemSettings or removing unrelated registrations. Windows
Settings owns its list UI: delivery is best-effort and immediate visual removal
must be verified on Windows, not claimed from a mock or guaranteed if Settings
ignores the notification. Successful MSI removal/registration absence and a live
UI refresh are separate acceptance checks.

Tests added: mixed_sut_removal_mock (actual atomic early gate/order/partial-result
contracts), apps_notification_mock (actual bounded notification); actual execution
plan tests cover SUT + each managed removal row, and vendor worker tests now cover
loaded SUT removal with native Settings scheduled for later removal. Full-removal
and cleanup-review harness seams were brought up to current Resources/Inno facts.
Profile/vendor refusal, MSI errors, restart results, plan matrix, scoped snapshots,
resource smoke and recommendation DOM tests pass. No MSVC/Windows installation or
Settings live-list acceptance was performed here.

Build GlassUnins Release|Win32 (solution x86), then GlassSetup Release|Win32
(solution x86). Publish updated embedded uninstallers in MSI distributions as
needed; this does not require rebuilding GlassShared or the OG vendor installer.

### Download preparation failure: GlassSetup-30

Setup FileVersion 2.0.0.254 downloaded all five Glass MSI payloads to their expected
sizes, but rejected each with SHA-256 mismatch (ERROR_CRC 23): Platform64 5222400,
Platform32 5111808, Settings64 69779456, Settings32 63193088, Resources 3444736 bytes.
Official OG job index4 succeeded (4049232 bytes). Separate metadata discovery for
OG/SUT reported GitHub HTTP403; that is not the cause of the five post-transfer
hash failures. Local cached MSI signatures were accepted, while absent adjacent
files under the original launch directory were normal discovery misses.

The failure proves bytes differed from the catalog snapshot, not a blanket network
outage. Manifest/artifact publication mismatch (e.g. rebuilding/signing/replacing
MSIs without regenerating all selected product entries) is a likely explanation,
not independently proven here: GitHub release asset digests/sizes were readable,
but downloading version.json for comparison was blocked by the sandbox's asset
host access. Do not bypass verification or substitute hashes automatically.

Regenerate the catalog from the FINAL signed MSI/Setup files with all five product
IDs and IncludeSetup, publish those exact bytes and version.json last. A partial
Setup-only catalog update preserves old MSI entries and cannot repair changed MSI
hashes. Restart Setup after correcting publication because the current launch
chain intentionally uses its inherited catalog snapshot; repeatedly clicking retry
with the same mismatching release cannot fix that snapshot.

Native footer failure copy is now concise RU/EN retry-or-cancel guidance, no
uncheck/reselect instruction. Full verifier mismatch diagnostics now include both
expected and actual SHA-256 (shared Setup/MSI verifier), preserving ERROR_CRC and
all trust gates. Actual footer/verifier mocks pass; Windows font/DPI layout remains
a visual acceptance check. Uploaded GlassSetup-30.log was deleted after analysis;
its conclusions are retained here, image-33.png was not deleted.

### Resources before official OG startup; GlassSetup-31 cache warning

The five supplied MSI logs all ended with code 0. GlassSetup-31 shows an early
managed-phase cache publication attempting HashPublishedPackage on Resources
before its MSI was installed. InstalledCacheDigest correctly refused the missing
installed baseline. A second publication after the Resources MSI succeeded, but
the first finish warning remained. The managed phase now omits the pending
Resources blob while its independent pre-OG snapshot is alive; it is published
only after its actual MSI and official OG result are confirmed. This does not
suppress real cache errors or weaken the installed-cache digest binding.

This section supersedes the previous OG-then-Resources execution order. Upstream
v3.0.2.3749 Scripts/OpenGlass.iss [Run] unconditionally invokes sc start OpenGlassHost;
there is no supported silent no-start switch. Therefore a selected pair must
install graphics BEFORE invoking the official OG EXE, not merely restart it after.
The sequence is prepare/validate both and capture independent Resources baseline,
managed Glass commit, Resources MSI, official OG installer/postcheck, Resources
baseline release and maintenance-source publication. The visual plan also lists
Resources before OG. No service startup-type/ACL manipulation or replacement of
upstream service ownership is added.

The signed graphics MSI accepts explicit GLASS_OG_INSTALL_PENDING=1 for this
pre-OG install. Setup supplies it only in its private graphics phase, for the
fixed Resources ProductCode and after preparing the verified official OG package.
Normal standalone Resources install still requires installed OG; maintenance,
removal and recovery remain available without OG. This property expresses an
explicit planned dependency, not a service/registration or payload-trust check.
The Resources MSI MUST be rebuilt/published with Setup for this order change;
an older MSI's launch condition will reject a fresh pre-OG graphics installation.

Resources MSI failure restores only its independent baseline and skips OG vendor
execution. A subsequent OG refusal/failure/cancel attempts to restore Resources;
OG itself remains outside Glass rollback, and committed managed changes are not
claimed as rolled back. Recovery failures retain the independent snapshot. The
baseline is not discarded immediately after a successful Resources MSI.

When OG is among recommendations, the x64 compatible Resources row is added to
that same explicit accept-recommendations batch, after the OG selection IPC. This
also recommends reapplying a healthy installed Resources package alongside an OG
update. The paired capability uses upstream remotePayloadVersion (not the retired
OG MSI targetVersion). Hidden/x86/disabled/conflicting rows are not auto-selected;
merely opening the page does not grant installation consent.

Regression: actual mixed dispatcher failure branches, actual Resources phase with
ASan/UBSan, explicit pending-property MSI truth table, source/cache-binding guards,
execution-plan/product-plan matrices, cache digest/diagnostics, official launcher,
and production RU/EN recommendations/Resources/download DOM tests pass. No actual
Windows/MSVC/WiX/Inno execution was performed here. Build Resources MSI Release|x64,
GlassUnins Release|Win32, then GlassSetup Release|Win32; regenerate the release
catalog after final signing and publish the matching artifacts.

The six supplied build/install logs were deleted after analysis; image-34.png was
retained. The confirmed earlier five-MSI release/catalog mismatch was a separate
publication issue, not the cause of this cache-timing warning.

### Installed-app follow-up: icon, official OG Change, retained Settings entries

Resources maintenance ARP customization now explicitly writes DisplayIcon to the
verified cached GlassSetup.exe,0. The MSI already has ARPPRODUCTICON, but Windows
Settings also needs a usable icon path after our non-WindowsInstaller ARP redirect.

Official OG Change integration is now implemented, superseding the earlier
"if possible" backlog item. Only the unique validated exact upstream Inno key is
opened, with a second inventory equality check before writes. A trusted cached
Setup is required. Only ModifyPath (--modify --product retired logical OG GUID)
and NoModify=0 are changed; no new OG ARP entry, uninstall override, service/ACL
change or broad registry search is performed. Refresh runs after confirmed vendor
install/uninstall as well as normal managed maintenance. A failed refresh is a
finish warning, not a false vendor rollback. Source/mock tests cover absence,
invalid inventory, changed inventory, missing cache, access/write failures and
unchanged vendor UninstallString. No Windows UI acceptance performed.

The reported two retained GlassSettings entries after full removal are NOT yet
root-caused or claimed fixed: no removal log accompanies this report, and MSI
registration versus orphan ARP key versus SystemSettings cached list cannot be
inferred from an entry's display name. Bounded notifications were already present.
Do not delete MSI registrations or ARP keys by display name, force-close Settings,
or use Win32_Product (which may initiate repairs).

Run the read-only scripts/Get-SettingsArpDiagnostics.ps1 after removal while the
entries remain visible; it prints current-context MSI state and exact known
Settings ProductCode keys in HKLM/HKCU 32/64-bit views. It does not enumerate other
users' MSI contexts. Capture its output and the corresponding GlassSetup removal
log before closing/reopening Settings; those distinguish actionable residual
registration from an already-removed product retained only in the UI.

Build GlassUnins Release|Win32 followed by GlassSetup Release|Win32. These ARP
changes do not require rebuilding Resources MSI; its package icon is unchanged.

### GlassSetup-32: incompatible Resources artifact rejected after managed commit

The supplied log at 15:56:46.849 records the correct
GLASS_OG_INSTALL_PENDING=1 command line. The Resources MSI server log confirms
receipt of that property at 15:56:47.230, then rejects LaunchConditions with the
OG-first message and exit1603. The previous Resources source's new expression
starts with GLASS_OG_INSTALL_PENDING = 1 OR; that expression cannot reject with
this property equal to1. Therefore the consumed MSI is incompatible with the new
pre-OG protocol (consistent with an older release MSI), not a missing Setup
command-line flag. It was acquired as version2.0.0.0115 source3 and then reused
as source4 from prepared storage. Numeric version/trust/hash alone cannot prove
this capability. Exact artifact provenance requires inspecting the MSI itself.

The four other managed MSIs committed successfully before Resources failed;
Resources baseline recovery succeeded and vendor OG never ran, hence the empty
OG directory. Do not describe this as full rollback or upstream installation
failure. Rebuilding only Setup is insufficient: rebuild/sign/publish the updated
Resources MSI and generate matching release metadata including openglassResources.
For a same-version replacement, use the new catalog snapshot in a NEW Setup run;
clearing trusted caches manually is neither necessary nor recommended.

Setup now reads the acquired MSI LaunchCondition table in its prepare-only
Resources phase, BEFORE any managed changes or vendor execution. It requires the
explicit new condition prefix and reports an outdated-package error otherwise.
It does not patch MSI tables, bypass launch conditions, change ordering to OG-first,
or weaken signature/digest/identity checks. MSI database inspection is read-only,
not MsiOpenPackage or an install/repair session. The release catalog generator
also refuses an old Resources artifact when openglassResources is selected.
Partial catalog operations preserving existing entries still require the publisher
to explicitly include Resources when replacing that asset.

Regression: compiled actual MSI capability reader (old/new/malformed/empty tables,
all API failure/handle cleanup), actual preparation refuses before snapshot/install,
mixed dispatcher, Resources MSI expression matrix and resource source smoke pass.
Windows/MSVC/WiX acceptance remains pending. Build Resources Release|x64,
GlassUnins Release|Win32, GlassSetup Release|Win32; generate the catalog only after
final artifact signing. This guard prevents a partial installation with stale
packages; it cannot make the old published MSI support the new execution order.

### Official OG Modify presence and complete WebView page-routing audit (image-36)

The official OG ARP Change command correctly passes our logical OG product GUID,
but MaintenanceProductAbsent incorrectly treated that GUID only as a retired MSI
identity. A current official Inno registration has no such MSI, so the early guard
claimed it was removed. That same early return bypassed WebView initialization
at the end of CompleteStartupIfReady, permanently leaving the native Error UI.

The OG presence guard now checks unambiguous official Inno inventory first, then
retained file receipt, then legacy MSI presence. Inventory/root-query failures are
not treated as proof of absence; discovery/trust checks remain responsible for
execution authorization. Other managed products keep the exact MSI guard.
This does not grant Modify authorization: trusted cached controller, signer and
shell/ticket/handoff validation still run. The existing Change link need not be
rewritten; rebuild/publish Setup and let its verified controller/cache update run.

Setup now detects WebView runtime availability even for duplicate launches and
queues UI-STA initialization before all startup terminal branches. Error and
AlreadyRunning wait on WebView when initialization is expected (native loading
placeholder, not permanent native error); an actual WebView failure still exposes
usable native controls. AlreadyRunning and RuntimeError map to the web error
state, with the appropriate localized labels/details rather than endless loading.
GlassUnins also initializes WebView for its duplicate-launch Error page instead
of deliberately skipping it, and detects failure to post the initialization.

Page coverage audit:
- Setup: all 23 Page enum values have explicit mappings. Welcome, Checking,
  Conflicts, Downgrade, VersionInvalid, License, Components, Certificate,
  FullCleanup, SettingsRemovalRisk, Installing, ModulesBusy, ProcessesStuck,
  ConfirmCancel, Cancelling, Cancelled, Launching, Complete and Error have WebView
  states. Installing preserves its dynamic unlock routing. AlreadyRunning and
  RuntimeError use the web error state if a runtime is available.
- RuntimeRequired/RuntimeInstalling necessarily use native bootstrap UI while
  WebView2 is absent (and RuntimeError likewise needs native fallback if runtime
  installation failed). Successful runtime installation initializes WebView.
  Absence/environment/controller/navigation failure and debug --demo-native
  remain intentional native fallback, not missing business-page coverage.
- GlassUnins: all 10 Page values mapped: Select, Cleanup, SettingsRisk, Stopping,
  UnlockModules, UnlockProcesses, Removing, Cancelled, Complete and Error.
- All 23 supported HTML modes are recognized; production DOM smoke visits each
  mode in RU/EN for Setup/GlassUnins without JS errors. Welcome asset loading is
  intentionally asynchronous. Enum routing and early-branch ordering are checked
  separately. These checks do NOT prove Windows rendering, focus, DPI/accessibility
  or live WebView2 initialization; actual Windows acceptance is still required.

Regression: actual stale-ARP guard covers official present/absent, inventory
failure, receipt, root failure and retired MSI; official Change adapter test;
full page routing and DOM smoke; maintenance entry/full maintenance DOM; source
resource smoke pass. The larger maintenance DOM test had a stale expectation
excluding verified absent products from accepted recommendations (superseded by
the earlier fresh-install recommendation change); expectation now matches that
explicit user-approved policy, with no production policy change in this patch.
Build GlassUnins Release|Win32 -> GlassSetup Release|Win32; no Resources MSI change.

### Open Installed Apps window retains pre-update commands (image-37)

User clarified that reopening Windows Settings fixes the links: current ARP data
is correct, but the same open window retains an old Installer/<digest>/GlassSetup.exe
command. Startup finalization collected non-active digest directories as soon as
they were unlocked. This made an externally cached command fail with file-not-found.
The existing bounded WM_SETTINGCHANGE broadcast is best-effort and does not prove
that SystemSettings rereads its per-window command cache.

Finalization now retains published content-addressed controller folders rather
than treating them as abandoned staging. Legacy Current code is likewise no
longer explicitly deleted in finalization just because ARP refresh succeeded.
Abandoned Stage-/snapshot cleanup continues. Old published controllers are removed
by explicit RetireUnused only after no products, no reboot pin and exclusive lease.
This trades disk space for command compatibility: multiple installed updater
versions can remain until retirement; no age/count-based deletion can guarantee
that an open external window has released the old command.

For newly built controllers, Modify authorization recognizes a retained protected
content-addressed Setup path as well as Active. Its actual SHA256 must equal the
folder name, the exact Setup filename/path and protected directory ACLs must match,
Active must still be valid, and signer plus parent/ticket/handoff checks still run.
Existing controller selection transfers to the newest compatible Active without
downgrade. This does not change older EXE code retroactively: already-deleted
old paths require refreshing the existing Settings window once; older retained
controllers predating the authorization fix may enter the normal wizard instead
of preserving Modify intent. New-to-new updates exercise the full compatibility
path. No registry redirection to untrusted paths, Settings force-close, or UI
refresh guarantee is claimed.

While checking retirement, Resources was found missing from HasProducts' fixed MSI
list. It is now included so keeping only Resources cannot retire its Setup link.
The old/new digest-store mock proves old code paths remain resolvable after
finalization (including once unlocked), while a separately simulated missing
rollback target still fails closed. Actual retained-entry identity/ACL/hash tests,
Resources-only lifetime tests, controller handoff, notifications and source smoke
pass. Real open-SystemSettings Windows acceptance is pending.

Build GlassUnins Release|Win32 -> GlassSetup Release|Win32; publish matching Setup
metadata. Resources MSI need not be rebuilt. The separately reported disabled
Apply button is still unconfirmed: no selected-plan screenshot or log accompanied
this report, and retaining command paths does not fix a selection/readiness gate.
Do not claim that issue resolved by this patch.

### GlassSetup-33: official OG reports restart despite successful startup

All five managed MSI install/reinstall logs (Settings64/32, Platform64/32,
Resources) ended0, and their uninstall portions also ended0. At16:40:36.723 the
official OG post-check records raw exit3010 with installed/path/version checks
all true for3.0.2.3749. The only restart finish note immediately follows. Setup
uses Inno /NORESTART /RESTARTEXITCODE=3010, so the source is the official vendor
installer's restart request, not our inference from service state or a managed
MSI request. Successful OG startup does not establish that deferred vendor file
operations have completed. Do not hide/rewrite3010 to0 merely because OG runs.

The exact reason/file cannot be recovered from the supplied Setup/MSI logs.
The launcher previously wrote OpenGlass-install.log inside its Work-* staging
folder, which normal cleanup deletes. It now requests GlassInstallLog's protected
per-launch directory; Setup's bridge now exposes InstallLog::Directory() instead
of returning empty. The vendor log therefore survives in Windows\\Temp\\GlassSetupLogs-*
with Setup/MSI logs. The raw exit and exact vendor log path are explicitly logged.
If persistent logging is unavailable, retain the former protected-workspace
fallback, recording persistent=0 rather than blocking the vendor operation.

RU/EN restart wording now explicitly attributes code3010 to the official OG
installer and explains that already running does not cancel a restart request.
It does not assert a particular locked/deferred filename without the vendor log.
For this completed run, restarting Windows is the conservative vendor-directed
action. If the request repeats, capture OpenGlass-install.log from the NEXT build
together with GlassSetup.log to distinguish pending file replacement from another
Inno restart reason. No service deletion/startup-policy changes, manual pending
rename cleanup, forced reboot or termination added. Tests exercise raw0/3010/2
preservation, persistent vendor log command and missing-log-directory fallback;
source smoke passes. No Windows/Inno execution performed here.

Build GlassUnins Release|Win32 -> GlassSetup Release|Win32. No MSI rebuild required
for these diagnostic/wording changes; publish matching Setup metadata.

### Resources removal Apply gating and mixed removal dispatch (image-38)

The screenshot's all-remove plan hits an obsolete ResourcesPlanReady condition:
Resources removal was disallowed when OG was also marked for removal. Removing
graphics requires no installed/retained OG dependency. The x64/no-OG-Apply gates
remain, but the !Removes(3) veto is removed. This does not implicitly select OG
removal when Resources alone is selected.

Simply enabling the button would have exposed a second stale restriction:
MixedOpenGlassRemoval accepted only OG removal plus managed Apply, not managed
removals. The dispatcher now also accepts OG removal plus any explicitly selected
managed removal subset and SUT removal (no vendor installation, force-clean or
unknown bits). It preflights official OG before managed mutation, excludes OG's
row from the managed transaction, runs actual selected MSI removals (including
Resources row64, no longer stripped from the procedure mask), then runs official
OG removal last. SUT retains its independently tested pre-Settings commit rule.
If the managed phase fails, vendor OG is not invoked. If OG subsequently fails,
committed managed changes remain a reported partial result, not a claimed overall
rollback. The existing stop/check/official uninstaller contract is unchanged.

Explicit optional profile cleanup is preflighted on the original plan and deferred
until official OG success; it is suppressed during the managed subtransaction so
it cannot delete retained OG settings early. Preflight now includes official Inno
presence, not only the retired MSI/receipt. Partial removal never implicitly asks
for profile cleanup. FullRemovalSelected's existing mixed-SUT cleanup eligibility
is not broadened by this patch.

The clarification says Resources-only removal also disabled Apply. A true pure
Resources-only plan already passes the extracted readiness predicate, so its
specific runtime blocker is not proven by the screenshot (which has seven planned
removals). New actual-IPC regression verifies that selecting Resources removal
clears its Apply flag, sets only bit64, requires no OG presence, and enables Apply;
all63 nonempty subsets of the six managed removal rows are covered, including
OG+Resources/all rows. The actual worker subset test also proves Resources-only
never calls the vendor OG lane. Runtime component-plan logs now record removeMask,
OG/Resources/SUT Apply flags, inspect/download readiness, failure gate and final
canApply, to distinguish a remaining standalone blocker without guessing.

Tests: actual IPC/readiness, mixed dispatcher/subset routing/failure/profile-cleanup
ordering, Resources UI matrix, standalone policy, actual SUT early gate/removal
worker, full-removal policy, both16384-case product-plan matrices and resource
smoke pass. A stale standalone source guard forbidding all ARP refresh was updated
to reflect the earlier official OG Change-link implementation. No Windows/MSVC/
WiX installation/removal acceptance performed. Build GlassUnins Release|Win32 ->
GlassSetup Release|Win32; no Resources MSI rebuild is needed for these changes.

### OG uninstall option correction; Settings ARP Remove availability

Latest user correction supersedes the earlier contradictory option wording:
our default official OG invocation uses /DELETEPRESETS /NODELETECONFIG;
with explicit registry-cleanup consent it uses /DELETEPRESETS, omitting
/NODELETECONFIG. Common /VERYSILENT /SUPPRESSMSGBOXES /NORESTART and the raw3010
restart mapping remain unchanged. Upstream v3.0.2.3749 InitializeUninstall confirms
that absence of /NODELETECONFIG enables config deletion and /DELETEPRESETS deletes
commonappdata/OpenGlass/Presets (user-created/downloaded packages, not merely an
opaque cache). This destructive preset removal is the user's explicit default
policy request; it is not inferred from an unrelated cleanup checkbox.

RunUninstaller takes a default-false deleteConfig option. Setup supplies its
explicit cleanupUserData consent after restoring the original mixed plan, before
vendor execution. Standalone GlassUnins uses the preserve-config default; its
existing prohibition on combining standalone OG with general all-profile cleanup
is not silently bypassed. Direct Installed Apps removal still invokes official
Inno and its own option UI; its UninstallString has NOT been rewritten/adopted.

All-profile installer cleanup now calls CleanPlatformSettings, not the legacy
CleanOwnedSettings API. The new scope leaves all OG/DWM values untouched, even
on preflight. OG cleanup belongs to the official uninstaller and covers HKCU of
the account running it plus HKLM, not every other user's hive. General maintenance
cleanup UI/log text no longer claims all-profile OG/DWM cleanup; Setup explicitly
states the official OG current-user/machine scope if OG is selected for removal.
The existing general RegistrySanity API and its non-installer callers retain
previous behavior. Glass-only themes/data/profile access safeguards are unchanged.

Settings ARP NoRemove is now always0 after a validated managed-entry refresh.
A broad vendor-presence flag previously disabled the Windows Remove button even
after Platform removal; ARP visibility is not removal authorization. GlassUnins
still performs its live helper/Platform/SUT/loaded-style checks, risk consent and
snapshot validation. The existing RemainingArpRefresh finalizer after Platform
uninstall rewrites both still-installed Settings architectures in their checked
contexts/views (and does not recreate absent Settings). Thus old NoRemove values
are cleared on the next refresh; no cross-architecture key copying is introduced.
Windows Settings may still cache button policy until it processes a refresh;
this is not a promise to force its already-open UI to redraw.

The separate observation that Resources remains in Installed Apps after direct
MSI removal, then disappears on a subsequent platform operation, is NOT claimed
fixed here. It is consistent with missing external UI refresh, but no registration
or removal log was supplied to establish whether an ARP key survived. Do not delete
MSI registrations/keys by display name or relabel it as successful live UI refresh.

Regression: exact default/cleanup vendor command assertions, raw exit/stop/trust
failure tests, standalone uninstaller test, actual scoped registry cleanup proves
no OG/DWM access, actual all-profile inventory, managed ARP writer/refresh proves
NoRemove0 across vendor-present/unknown and legacy contexts plus refresh failures,
and source smoke pass. The ARP mock now isolates the three independent vendor/file/
Resources adapters (covered by their own compiled harnesses), rather than failing
on unrelated newly added adapter dependencies. No Windows integration performed.
Build GlassUnins Release|Win32 -> GlassSetup Release|Win32 (normal shared library
dependencies rebuild); these changes require no MSI package rebuild.

### Mixed-removal preflight and diagnostics

Official OpenGlass removal now verifies the current Inno registration, protected
paths, pinned EXE/data and publisher during plan preflight, before managed MSI
mutation. The runner repeats validation immediately before service preparation.
Trust policy is unchanged; WinVerifyTrust failures include the numeric status.
A mixed plan retains its managed rollback snapshot until the official operation
succeeds; partial completion preserves and logs the snapshot path. This is not
an automatic rollback of the official uninstaller.

The plan counter reports selected actions, not selection out of catalogue rows.
The optional full Glass-data cleanup review is available when every installed
Glass component is being removed even if SecureUxTheme is retained. The final
Glass-registration guard excludes retained SUT; no SUT data is cleaned.
Successful MSI removals log all-context registration enumeration, MSI state and
both HKLM ARP views without deleting registry entries.

Regression coverage includes extracted C++ official-runner, mixed-worker,
full-removal policy, maintenance lifetime and removal-registration diagnostics,
plus production DOM action counts. These checks are not a Windows native build
or proof of immediate refresh of an already-open Installed Apps window.

### Universal registry-cleanup review in the consolidated setup plan

One common page appears before execution when OG removal or complete installed
Platform-architecture removal is selected. It lists only eligible products:
Platform, OpenGlass, or both. Two independent unchecked-by-default choices apply only
to their respective eligible products on that same page. Product changes reset consent and page review.

Internally the product flags remain scoped: OG consent alone controls omission
of `/NODELETECONFIG`; `/DELETEPRESETS` remains unchanged. Platform consent uses
registry-only profile preflight/commit, never saved-theme or AppData cleanup.
Retained Settings, Resources or SUT do not suppress the page. Platform commit
checks absence of current/legacy Platform registrations. Native and WebView
interfaces use the same eligibility and independent consent mapping.

The separate legacy GlassUnins selection/cleanup UI is not changed by this patch.


### Log 35: missing uninstaller signature and cleanup UI regression

WinVerifyTrust status 2148204800 is 0x800B0100, TRUST_E_NOSIGNATURE.
This log shows refusal during preflight, before any MSI mutation, not a failed
MSI batch or generic Windows policy prohibition. The error now identifies the
missing verifiable signature and avoids ERROR_INSTALL_PACKAGE_REJECTED's
misleading policy heading. Unsigned official uninstaller support is unresolved;
this patch does not bypass publisher verification.

The universal review now uses two compact product-specific checkbox cards,
explicit pointer interaction, a bounded scrollable content area and no background
illustration under the text. Chromium hit-testing/geometry regression covers
RU/EN, single-product/both-product states and independent IPC. DOM tests cover
selection independence. Recommended OG now carries Resources in initial choices;
paired recommendation capability no longer inherits unrelated current intents.
The plan counter reads “Изменений в плане” / “Planned changes”.

Cleanup review heading follow-up: confirmation pages use the shared decision-page
heading grid (left 48px, top 78px), matching the certificate page. The header
must never be centered or distributed based on body height/product count.
The previous vertically centered layout is superseded. Checkbox cards remain
below the fixed header, with no mouse-click outline around the whole card;
keyboard focus remains visible on the checkbox. Contextual preservation hints
use singular copy for single-product plans.
Chromium regression compares the actual cleanup title coordinates against the
certificate title and asserts identical cleanup heading coordinates across
RU/EN and all single/both-product states, in addition to click/IPC/bounds checks.

### Maintenance navigation and operational heading grid

Maintenance rejects Back at the Components boundary, and SetPage redirects any
maintenance transition to License/Welcome back to Components. This guards
keyboard/programmatic requests as well as the already hidden Back button.

Operational WebView2 pages now share the heading anchor, including Components,
License, Certificate, confirmations, checking/conflict/version gates, completion,
error, cancellation, launch/uninstall and module unlock. Page-specific 65px/64px
and margin/inline-layout overrides were removed or normalized. Slideshow artwork
and loading splash are separate layouts. Chromium compares title x/y against
the certificate reference across 16 states and both locales; module-holder layout
regression still passes.

The cleanup page remains in the persistent offline WebView2 document. It now
adds a restrained CSS-only backdrop, decorative noninteractive outline shapes,
and a contextual paragraph explaining cleanup timing/ownership for the eligible
products. No remote background assets, extra navigation or consent changes.
Windows native execution and installer acceptance still require Windows testing.

### Logs 1/2 (Setup 2.0.0.263): cleanup root rights and stop cancellation

Cleanup preflight refused HKU SID ...-1008 with access denied before MSI changes.
General profile inventory unnecessarily requested KEY_READ|KEY_WRITE on the
loaded hive root. It now requests KEY_READ; cleanup still separately verifies
write access on owned Platform keys and refuses actual subkey denial. No ACL
changes or inaccessible-profile skipping. Mock coverage includes denied root
write with successful registry-only cleanup, plus owned-key denial refusal.
The cleanup error uses access-denied wording, not install-policy rejection.

Log 2 ends after cooperative x64 stop and cancellation, without MSI execution.
Cooperative stop now sets the stop-attempt flag before signaling, retains it
through cancellation/task-stop handling and therefore takes the existing
wait-for-stopped restoration path before restart. Entry/pre-signal cancellation
checks avoid starting a stop after cancellation during validation. Additional
stop/restore diagnostic lines aid reproduction. This is not proof that the
reported Explorer crash is fixed; fault module/exception/event or dump is needed.

Cancelled-removal WebView uses removal-specific copy and hides an empty detail
callout. DOM tests cover both empty/nonempty details. Profile inventory mocks,
source ordering, source smoke and Chromium layout tests pass. Two older stop
mocks currently fail for unrelated stale harness expectations: unins_task_stop
expects a removed EnsureModulesReleased token; task_scheduler_stop lacks
OpenMutex/handle/error API stubs. No native Windows build/runtime validation.
