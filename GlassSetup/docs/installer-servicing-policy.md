# Единая политика обновления и обслуживания GlassPlatform

Статус: целевая архитектура по запросу пользователя; не отчёт о реализованном self-update.
Реализация текущего handoff отсутствует. Не включать его одним ShellExecute.
Первый runtime-инкремент: разделены repair-source coverage и ошибка публикации
существующего комплекта; controller/source-store физически ещё не разделены.

## 1. Основание и границы

Пользователь поручил выбрать технические решения по модели Visual Studio Installer,
без дополнительного согласования расположений/кэшей/передачи управления.
Это заменяет необходимость выбирать между вариантами предыдущего обсуждения Log24.
Общие прежние ограничения сохраняются: не копить полные старые комплекты,
сохранять только нужные источники, не менять незапланированные продукты,
не ослаблять проверку доверия и не трогать системный Windows Installer cache.

Документация Microsoft подтверждает разделение bootstrapper и установленного
Installer, отдельный package cache и Windows Installer source management.
Она НЕ задаёт универсальный протокол self-update стороннего EXE, конкретный
путь GlassPlatform, наш SHA256 receipt или нашу схему JSON. Ниже это решения
GlassPlatform, основанные на документированных правилах, а не «сертификация Microsoft».
Внутренний порядок TEMP/процессов конкретной версии VS Installer не объявляется
известным по наблюдению UI или копируется как недокументированный контракт.

Источники:
- https://learn.microsoft.com/en-us/visualstudio/install/install-visual-studio?view=visualstudio
  Bootstrapper устанавливает latest Visual Studio Installer, отдельную программу.
- https://learn.microsoft.com/en-us/visualstudio/install/use-command-line-parameters-to-install-visual-studio?view=visualstudio
  Self-update Installer отдельно от продуктовых команд; documented cache/nocache,
  noUpdateInstaller и ошибки при обязательном обновлении.
- https://learn.microsoft.com/en-us/visualstudio/install/disable-or-move-the-package-cache?view=visualstudio
  Package cache нужен для offline repair; недостающие пакеты могут скачиваться.
- https://learn.microsoft.com/en-us/windows/win32/msi/windows-installer-best-practices
  Secure/available original sources, корректные PackageCode, управление SourceList
  через API; постоянные source files не должны находиться в системном TEMP.
- https://learn.microsoft.com/en-us/troubleshoot/windows-client/application-management/missing-windows-installer-cache
  Системный Windows Installer cache нельзя удалять/подменять вручную.
- https://learn.microsoft.com/en-us/windows/win32/api/msi/nf-msi-msisourcelistsetinfow
  SourceList учитывает product/patch, context, SID, имя и путь пакета.
- https://www.microsoft.com/en-us/msrc/blog/2018/04/triaging-a-dll-planting-vulnerability
  Запуск из недоверенных каталогов и DLL planting; private random staging и
  защищённый установленный code directory вместо доверия Downloads/CWD.

## 2. Три независимо версионируемых слоя

1. Внешний GlassSetup.exe — entry/bootstrapper. Можно запустить из Downloads,
   layout, съёмного носителя. Его каталог не становится доверенным постоянным
   controller location или зарегистрированным repair source автоматически.
2. Установленный controller — актуальные совместимые Setup/Unins и launch/update
   support. Обслуживает фактически зарегистрированные поддерживаемые продукты,
   включая разные версии Platform/Settings. Версия controller не равна версии
   установленной платформы и не требует её обновления.
3. Sources — оригинальные MSI/CAB для конкретных установленных package identities.
   Новый controller не делает новые MSI подходящими для repair старого продукта.
   Offline repair возможен только при наличии полного подходящего source payload.

Системный LocalPackage в Windows Installer — четвёртый, ОС-владеющий слой.
Не путать его с оригинальным distribution MSI/CAB и не использовать как
универсальную замену полной исходной поставки.

## 3. Размещение (решение GlassPlatform, не предписание Microsoft)

- Controller: отдельный защищённый machine-wide code directory, логически
  `%ProgramFiles%\GlassPlatform\Installer`, не внутри выбранного каталога продукта.
  Использовать Known Folder API с учётом x86 Setup/x64 OS, не строковый WOW64 путь.
- Активный controller выбирается стабильной защищённой точкой запуска/записью
  состояния; binaries публикуются в отдельное immutable version/hash directory.
- До подтверждённого запуска новой версии сохраняется один recovery controller.
  После успешного handoff старый удаляется, когда освобождены файлы/handles/pins.
- `%ProgramData%\Packages\GlassPlatform` остаётся data/source/work store с
  admin/SYSTEM write ACL, а не растущей историей полных Setup bundles.
- Sources адресуются identity + content digest, дедуплицируются; manifest хранит
  installed references и временные transaction/reboot/recovery pins.
- Staging/work — приватные случайные каталоги с проверкой владельца/ACL и отсутствия
  reparse points. Общий пользовательский TEMP не является elevated trust boundary.
  Если TEMP используется для первичного extraction, elevated процесс повторно
  проверяет и переносит код в защищённое staging перед выполнением.
- Код запускается по абсолютному пути, с контролируемым DLL search и CWD;
  не подхватывать соседние DLL из Downloads/layout.

## 4. Цепочка запуска и self-update

1. Разобрать только поддерживаемые аргументы; определить intent install/modify/
   repair/remove/product/context, язык. Не начинать MSI, profile reset, остановку
   платформы или service mutation в bootstrap phase.
2. Найти установленный controller через доверенную machine-wide регистрацию.
   Проверить путь, ACL, reparse, подпись и допустимую identity. Имя процесса или
   родитель explorer сами по себе не являются разрешением на privileged handoff.
3. При online доступе получить release channel и проверить publisher/manifest,
   hash/size/version/architecture/schema compatibility предлагаемого controller.
   Источник загрузки не меняет автоматически разрешённый publisher/channel.
   Старый downloaded bootstrapper не понижает уже установленный controller.
4. Если installed controller уже новее/равен и совместим — передать intent ему.
   Если есть более новый допустимый — скачать в private work, проверить до/после
   копирования, подготовить полный controller (Setup/Unins/support одной версии).
5. Отдельный updater/launcher, не использующий заменяемые файлы, берёт controller
   lock. Записывает durable journal, публикует новый version directory, атомарно
   меняет active pointer. Не перезаписывает исполняемый EXE на месте.
6. Новый процесс запускается из постоянного защищённого controller directory,
   не из случайного download TEMP. Передача содержит versioned ограниченный intent
   и одноразовый защищённый ticket; не сериализует доверие к путям/MSI или прежнему
   inventory. Parent PID без process identity/nonce недостаточен.
7. Child проверяет ticket, publisher/active identity, схему и readiness, подтверждает
   приём. Только после ACK прежний UI закрывается и отдаёт operation ownership.
   Нельзя ждать child, удерживая lock, который child обязан взять для ACK.
8. Child заново инспектирует все MSI/context/SID и источники, строит UI/план.
   Для нового запуска не воспроизводить Apply автоматически: выбор продукта не
   равен согласию на новую MSI-транзакцию. Maintenance intent не теряется.
9. Readiness/launch failure до product mutation возвращает active pointer на
   предыдущий controller и показывает ошибку обновления; никакого downgrade loop.
   Ограничить цепочку переходов и хранить attempt ID/target hash.
10. Pending journal восстанавливается при следующем запуске после crash/reboot.
    Удалять abandoned staging лишь после проверки ownership, journal и pins.

Обновление controller самостоятельно commit-ится ДО продуктовой транзакции.
Поздняя ошибка установки продукта не откатывает успешно обновлённый controller.
Это устраняет зависимость «новый Installer только после полного product success».
Обновление controller не обновляет Native установленного GlassSettings: зависимости
конкретных операций (включая SecureUx removal helper) остаются явными.

## 5. Online/offline и совместимость

- Online: автоматический безопасный self-update перед продуктовым UI, если доступен
  более новый совместимый controller из разрешённого канала.
- Offline/403/timeout: не объявлять установленный controller повреждённым; работать
  существующим доверенным совместимым controller и проверенными local/layout
  источниками. Не блокировать uninstall лишь из-за недоступности сервера.
- Если пакет/схема требует более нового controller, блокировать конкретную
  неподдерживаемую операцию с явной причиной и offline способом обновления.
- Нельзя обещать поддержку «всех когда-либо выпущенных продуктов»: вести явную
  таблицу supported product families/schema/contexts и безопасно отказывать unknown.
- Version/product state/UI source rows сохраняются раздельными; ни self-update,
  ни source acquisition не auto-select Platform/Settings/OpenGlass или SUT.

## 6. Продуктовая транзакция и кэш источников

Перед MSI проверять полный нужный source, подпись/разрешённую identity, hash и
безопасность paths; хранить transaction reference. После MSI перечитать фактическую
регистрацию через Windows Installer API. Receipt связывает distribution source
с фактически установленной package identity/context; текущая пара SHA256 является
нашей проверкой происхождения, а не обязательным форматом Microsoft.

- Cache exact originals для установленных продуктов по умолчанию: offline repair
  не должен неожиданно зависеть от исходной флешки/Downloads.
- При частичном update не заменять source неизменённого продукта MSI другой версии
  и не сохранять для него целый старый controller.
- Источники регистрируются через MSI API в соответствующем context/SID, не прямым
  редактированием Installer registry. Постоянный SourceList не указывает в Work/TEMP.
- Перед GC заменить только собственные SourceList references; не стирать чужие.
- Удалять лишь наши sources без installed/transaction/recovery/reboot references.
  Не чистить `%WINDIR%\Installer` и не удалять vendor permanent SUT files.
- Ошибка GC даёт CleanupPending, не rollback уже успешного product/controller.

### Legacy migration

Старый Current не становится доверенной поставкой оттого, что подписан, расположен
в ProgramData или имеет такой же ProductVersion. Инвентаризировать registrations,
source candidates и существующие receipts. Переносить подтверждённые originals;
unknown кандидаты не регистрировать и не уничтожать последний возможный источник.
Для восстановления покрытия нужен exact original из проверенной поставки/layout
или явное product reinstall; не подменять его «самым новым» MSI.
Миграция controller/ARP не должна ждать полного offline repair coverage.
При отсутствии источника отображать ограничение именно этого продукта. Не требовать
переустановки всех продуктов только ради контроллера и не обещать repair без CAB.

## 7. Раздельные результаты и финальный UI

Хранить независимо:
- Controller: Ready / UpdateFailed / RecoveryPending;
- Product transaction: Success / Failed / Partial / RebootRequired;
- Source coverage per product: VerifiedLocal / VerifiedRemote / NeedsOriginalSource;
- Cleanup: Complete / Pending.

Log24 должен означать: SUT removed, reboot required; controller files опубликованы,
coverage Platform64/32 не подтверждён. Не выдавать coverage warning за «не удалось
сохранить оболочку», не откатывать успешный MSI и не держать полные Previous bundles
навсегда из-за отсутствия одного legacy receipt. В текущем коде controller launch
health отдельно не проверяется, поэтому нельзя ретроспективно назвать его Ready.

## 8. Аудит текущей реализации и обязательные этапы реализации

Проверено по HEAD 4455795:
- main.cpp CompleteVersionCheck скачивает/проверяет Setup, но явно НЕ включает
  automatic publication/handoff. Нельзя утверждать, что self-update уже работает.
- GlassPackageAcquisition private Work/signature/hash — полезная существующая база.
- LaunchSelectedProducts публикует весь cache только после package apply; removal-only
  оставляет controller прежним. Это не удовлетворяет независимому self-update.
- GlassMaintenanceCache Publish смешивает Setup/Unins, MSI и repair SourceList;
  activate-current происходит раньше source registration; отказ legacy binding
  оставляет Previous и общий incomplete warning. Это причина Log24.
- GlassMaintenanceLaunch/ARP привязаны к Current/GlassSetup.exe; перенос controller
  требует миграции всех вызывающих сторон/maintenance tickets, а не одной строки.
- Локальная доступность newer binary не является ACK/успешным запуском.

Порядок инженерных работ (не выдавать за выполненный код):
1. Versioned controller manifest/journal + pure transition model и crash-point tests.
2. Secure publisher/updater + child readiness/ticket/lock handoff, offline/rollback.
3. Перевести downloaded и installed entrypoints/ARP/Settings на единую цепочку;
   снять заглушку CompleteVersionCheck только после реализации шагов 1–2.
4. Отделить source-store receipts/coverage/GC от controller publish; мигрировать
   legacy Current, не теряя неподтверждённые возможные originals.
5. Раздельные RU/EN результаты, убрать ложный общий warning для coverage gap.
6. Windows acceptance: x86 Setup/x64 OS, initial/partial/removal-only, old external
   bootstrapper/newer installed controller, blocked network/403, unsigned/substituted
   binary, crash до/после active swap и ACK, simultaneous launch, UAC cancellation,
   locked files, stale ticket/replay, product failure после controller commit,
   exact old repair sources, last-product removal, source GC/reboot pins.

Эта спецификация сама по себе не выполняет ни один runtime migration/handoff.
Не включать небезопасный промежуточный автозапуск ради демонстрации UX.


## 9. Реализованный первый инкремент

RegisterRepairSource теперь возвращает отдельно список NeedsOriginalSource для
подходящих установленных identities без проверенной binding. Он не вызывает
MSI SourceList API для таких файлов и не заявляет их доверенными. Candidates
продолжают копироваться/удерживаться согласно прежней installed identity policy.
Реальные ошибки trust/identity/context/MSI SourceList API остаются ошибками.

Publish передаёт coverage отдельно от publication error: legacy coverage gap сам
по себе больше не удерживает полный Previous. Current/Retained candidates уже
сохранены до удаления Previous. При реальном registration failure Previous
сохраняется как раньше. Final RU/EN warning называет отсутствие repair sources,
не ложный отказ оболочки. Выполненные продукты не откатываются из-за этого.

Этот первый инкремент сам по себе не включал controller lifecycle. Следующий
runtime-этап описан ниже; Windows acceptance раздела 8 остаётся обязательным.
Старые registration/GC политики не объявлены полноценным reference-counted source-store.

## 10. Подключённый runtime controller (2026-11-04)

- До продуктовых действий Checking выбирает максимальную доверенную совместимую
  версию self/Active/download. При равенстве остаётся установленный Active.
  Повреждённый существующий Active и недоверенный скачанный EXE — отказ, а не downgrade.
- Защищённый `Installer/<SetupSHA256>` содержит только Setup и извлечённый из его
  ресурса Unins. Оба проходят hash/publisher/Authenticode и проверку архитектуры.
  `Active`, `Previous`, `Pending` — строгие 128 ASCII hex (два SHA256), атомарная
  публикация через flush + rename. MSI Current/Retained и receipts независимы.
- Новый процесс запускается только из store, через CreateProcess с whitelist трёх
  inherited handles. Проверяются реальный родитель, его publisher, canonical self,
  handles и record. READY означает созданное окно; родитель ждёт до 30 секунд,
  публикует Active и разрешает COMMIT. До COMMIT message loop не запускается.
  Финального ACK успешного startup после COMMIT протокол пока не содержит.
- В child передаются только служебный протокол, разрешённый modify/product intent,
  no-self-update и source-layout hint. Согласие EULA и продуктовые действия не
  пересылаются. Hint не отменяет независимую проверку каждого MSI.
- Cache resolver, ARP refresh и Settings используют независимый Active; legacy
  Current допускается только при отсутствии Active. Старые entrypoint EXE
  удаляются после успешного ARP refresh; MSI/CAB/receipts не удаляются этой очисткой.
- Успешный canonical startup удаляет старые плоские code directories; занятые
  файлы получают CleanupPending и повторную попытку при следующем запуске.
  При startup failure восстанавливается проверенный Previous, если он есть.
  Pending не является авторитетным указателем и очищается при успешном startup.
  Если lock ещё у родителя, finalizer откладывается до следующего запуска.
- Offline работает с доверенным локальным controller; доверие к сертификату должно
  быть уже установлено. Authenticode использует cache-only chain retrieval без
  сетевой revocation проверки. Автоматического добавления сертификатов нет.
- Последний product removal не удаляет независимый controller: отдельная команда
  его удаления пока не реализована. Первый startup failure без Previous не имеет
  rollback-target. Полного crash-journal replay и reference-counted source GC нет.

Проверено actual-source mock-тестами с поддельными Win32 API, включая staging,
atomic pointers, handoff, child barrier, trust gate, rollback и locked-code GC.
Это **не** Windows/MSVC/WiX/.NET сборка и **не** проверка реальных ACL/AuthentiCode,
UAC, process races, reboot/crash recovery или MSI acceptance. Полную политику
нельзя считать принятой до выполнения Windows-матрицы раздела 8.
