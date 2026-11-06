# Сохранённые .theme включены в добровольную очистку — 2026-11-06

**Новое прямое уточнение пользователя:** критерий — `.theme` с секциями Glass в специальном пользовательском каталоге
сохранённых тем, у всех локальных профилей, не `Windows\Resources\Themes`. Если активна такая тема, сначала пытаться
переключить на стандартную; при неудаче разрешена прямая правка реестра соответствующего профиля. Ledger происхождения
для этого критерия не требуется. Это уточняет/заменяет прежнее открытое решение о file ownership.

- `GlassSavedThemeCleanup.h` выполняется в существующем GlassUnins, новых EXE/DLL нет. Batch каждого профиля
  подключён к имеющемуся Inventory и отдельной добровольной cleanup странице. Без согласия cleanup ничего не запускается.
- Сканируются только файлы верхнего уровня `LocalAppData\Microsoft\Windows\Themes\*.theme` с секциями
  `[Glass]`/`[Glass.*]` (case-insensitive). UTF-8/ANSI с ASCII headers, UTF-16 LE/BE с BOM, whitespace/comments учтены;
  упоминания в комментариях/значениях и похожие имена секций не являются критерием удаления.
  Темы без разделов Glass, `.msstyles`/wallpaper/libraries, системный каталог и пользовательские `.bak` не удаляются.
- Preflight проверяет доступ/DELETE rights, размер/число файлов, single-link/no-reparse и final canonical path;
  держит identity handles без разрешения rename/delete, но допускает Windows style preparation writes.
  Перед commit отдельный read seal исключает writers, содержимое читается заново. Если Glass секции исчезли,
  файл сохраняется. Другой активный writer, read-only/повреждённые файлы или unsafe paths дают отказ, не скрытый kill.
- Активная ссылка определяется по file identity, а не только по строке пути (в том числе aliases).
  Live SetSystemVisualStyle допускается только при совпадении SID профиля, process token и desktop user.
  Записываются стандартные CurrentTheme/ThemeManager значения в нужный HKU/private hive, не в произвольный HKCU.
  Для offline/несовпадающего desktop либо неудачного live apply используется явно разрешённый registry fallback.
- Registry fallback отражён в RU/EN finish warning HTML, native status и журнале. Запись конфигурации не выдаётся
  за подтверждённую live смену оформления; возможное применение при следующем входе не сопровождается logout/kill.
  Если запись stock links не удалась, соответствующий файл не удаляется. Поздняя ошибка удаления — partial result,
  повторного скрытого commit нет.
- Local AppData читается из hive профиля. Redirect внутри профиля поддерживается; redirect за его границы и неизвестные
  пользовательские environment variables блокируют preflight для ручной проверки, а не разрешают удаление чужих путей.
  Стандартные Windows resource paths проверяются на наличие/no-reparse перед переключением.

Проверено: `saved_theme_cleanup_mock.py` исполняет реальный parser/Batch с fake Win32 sharing/FS/registry/style APIs,
ASan/UBSan: encodings, false positives, multi-profile hives, active aliases, failed live apply → registry,
changed content, competing writer, hardlinks/reparse/final-path mismatch, readonly, partial deletion и handle lifetime.
`profile_inventory_mock.py` отдельно проверяет wiring theme preflight/commit failures через adapter stub;
не подменять эту проверку заявлением о Windows API runtime. Новый header и Inventory compile check собираются в
Windows GNU x86 object. MSVC/WiX/реальный Windows theme apply/delete здесь не выполнялись.

**Оставшиеся ограничения:** это не завершение всего cleanup/lightweight scope. Очистка профильных папок/логов/дампов,
остатки «только состав/пути», новый maintenance UI и общая транзакция ещё не завершены. Консервативные SUT preflight/
Native deactivate ограничения для неизвестных/кастомных стилей других профилей этим кодом не обходятся: весь совместный
SUT+cleanup сценарий с новым registry fallback требует дальнейшего подключения к общему порядку операций и Windows проверки.
Платформа/службы здесь не запускались/не останавливались.

# Повторная проверка состава профилей перед cleanup commit — 2026-11-06

- Inventory повторно читает ProfileList в конце preflight и непосредственно перед первой записью cleanup.
  Новый/исчезнувший/повторно перечисленный SID, изменённый ProfileImagePath или ошибка чтения блокируют commit.
  Перестановка порядка перечисления сама по себе не считается изменением состава.
- Повторно проверяются доступность и отсутствие reparse traversal у каталогов профилей и NTUSER.DAT.
  Уже открытые hive handles не закрываются/не перезагружаются для recheck: исключён конфликт повторного RegLoadAppKey
  со своими private mappings. Повторно открытые ключи ProfileList/entry закрываются при отказах.
- Отказ на этом этапе не пишет пользовательские/HKLM настройки и расходует one-shot разрешение cleanup;
  для повторной попытки нужен полный новый preflight. Само удаление продуктов к этому моменту могло уже завершиться,
  поэтому существующий UI честно возвращает незавершённый общий результат, а не обещает rollback удалённых MSI.

Проверено: реальный VerifyInventory и вызывающие его preflight/commit на fake API с ASan/UBSan:
новый профиль во время preflight, добавление/удаление/смена пути после preflight, duplicate SID, новый reparse,
исчезновение NTUSER.DAT, ошибки registry, отсутствие записей и утечек handles, разрешённый reorder.
Windows GNU x86 object с реальным Win32 API header повторно собран; resource/cleanup guards PASS.
Это не Windows runtime и не атомарная блокировка ProfileList от административных изменений во время самой очистки.
Безопасный ownership сохранённого `.theme` и реальные Windows профильные сценарии остаются открытыми;
весь lightweight план по-прежнему не завершён. Платформа не запускалась/не останавливалась.

# Профильная очистка: закрыты молчаливые пропуски и повторный commit — 2026-11-06

- ProfileList теперь определяет состав локальных профилей независимо от успешного LookupAccountSid.
  Удалённый аккаунт, недоступный домен или неразрешённое имя не являются основанием молча пропустить локальные данные.
- Недоступный/отсутствующий каталог профиля, NTUSER.DAT, reparse points и неверные типы file/directory блокируют
  preflight до продуктовых изменений, в том числе для уже загруженного hive. Частично собранный inventory сбрасывается,
  уже открытые handles закрываются. Устаревшая ProfileList запись с отсутствующим каталогом тоже блокирует операцию;
  никакой автоматической правки/удаления пользовательского профиля не добавлено.
- ProfileImagePath/ThemeManager строки проверяются на выравнивание размера, тип обеих фаз registry read,
  обязательный терминатор и отсутствие скрытого payload после embedded NUL. Повреждённая строка не становится
  укороченным допустимым путём.
- Cleanup требует успешного preflight всех профилей и HKLM и допускает только один commit. Другой сеанс,
  ошибка или частичная очистка потребуют нового inventory/preflight; повторный вызов не продолжает скрыто удаление.
  Повторный OpenAll также сбрасывает разрешение commit. Чужие сеансы не завершаются.

Проверено: `profile_inventory_mock.py` исполняет реальные ReadString/SafePath/OpenAll/PreflightCleanup/Cleanup
на fake Win32 API с ASan/UBSan: offline/loaded hives, второй недоступный профиль, ошибки registry/files,
повреждённые строки, reparse родителей, сеансы, частичный commit, отсутствие разрешения и освобождение handles.
Header и реальные вызовы PreflightCleanup/Cleanup скомпилированы в Windows GNU x86 object без API stubs.
Это не MSVC-сборка, не реальный RegLoadAppKey/профильный Windows runtime.

Следующий незакрытый пункт очистки: безопасное ownership и учёт сохранённого `.theme`; также остаются
повторная проверка изменения состава ProfileList между preflight/commit и Windows matrix. Это не объявляется
завершением всего профильно-файлового scope или общего lightweight плана.

# Отдельная страница добровольной очистки — 2026-11-06

Реализация всего lightweight плана **ещё не завершена**.

- В GlassUnins cleanup checkbox удалён со страницы выбора продуктов. Для полного удаления всех обнаруженных
  продуктов добавлена отдельная `cleanup-review` страница существующего HTML и native fallback.
  Карточки выбранных продуктов на ней только для чтения; Select all скрыт, очистка по умолчанию выключена.
- «Назад»/отмена возвращает к выбору и сбрасывает согласие. Изменение состава тоже сбрасывает согласие.
  Checkbox сообщения принимаются только на cleanup странице. Продукты на ней изменять нельзя.
- Пустой состав/одни остатки не могут открыть cleanup-only операцию. Частичный выбор не предлагает очистку.
  Silent cleanup не разрешён. Backend требует просмотр страницы и полный набор удаляемых продуктов.
- Перед стартом дополнительно проверяются невыбранные MSI identities во всех пользовательских контекстах;
  новая регистрация или ошибка обнаружения блокирует cleanup-план до изменений. Имеющийся preflight профилей
  остаётся до остановки хостов/MSI. Перед самой очисткой повторно подтверждается отсутствие всех семи MSI identities;
  ошибки/неизвестные состояния закрывают очистку. Предшествующая ошибка удаления выходит без cleanup.
- Тест `cleanup_review_mock.py`: 4096 сочетаний состава/выбора, реальные full-removal/absence функции на fake MSI API,
  ошибки enumeration/state, исключение выбранных продуктов на preflight, source contracts UI/consent/order, ASan/UBSan.
  Это не Windows runtime/проверка визуального размещения.

**Открыто в очистке:** учёт сохранённого `.theme` с безопасным ownership, полный аудит all-profile inventory и остатки
  «только состав/пути» в новом maintenance UI. Существующий профильный adapter не объявляется законченным всем scope.
Сам общий maintenance UI, удаление embedded payloads/Native, optional trusted helper fallback, sole Current,
self-update и независимые Settings notices остаются открытыми. Платформа здесь не запускалась/не останавливалась.

# Независимое удаление Settings и предупреждение Platform — 2026-11-06

Рефакторинг всего плана **ещё не завершён**. Пользователь сообщил об успешной сборке x64/x86;
точный SHA его локального checkout не подтверждён. Это не подтверждение выполнения Windows runtime-тестов.

- GlassUnins теперь различает Settings x64/x86: отдельные карточки и native fallback, точный выбор ProductCode
  при прямом запуске, отдельные MSI probes/removal и сохранение каталога оставляемой архитектуры.
  Ранее один settings checkbox мог удалить обе архитектуры даже при entry конкретного продукта.
- При удалении Settings с оставляемой соответствующей Platform до запуска worker появляется собственная RU/EN
  страница предупреждения в существующем мастере (и native fallback). Остановка Platform не приравнивается к её отсутствию.
  Другая архитектура не вызывает предупреждение. Объяснены риск падения процессов, порядок Platform → Settings
  и возможность отменить и использовать «Изменить». Автоматический запуск другого мастера/каскад не добавлен.
- Отмена возвращает к выбору без изменений. Продолжение явно принимает только показанные пары архитектур.
  Worker повторно читает MSI presence перед остальными действиями, новая пара не наследует старое согласие.
- Silent требует отдельный `--accept-settings-removal-risk`; флаг переносится в private relaunch.
  Старый запрет silent Settings removal при SecureUxTheme не ослаблен этим флагом.
- В совместном удалении Platform MSIs и работа с shell/helper выполняются раньше Settings MSI.
  Общая Setup/Unins mixed-action транзакция и две Settings карточки самого Setup всё ещё не завершены.

Проверено: `settings_removal_mock.py` исполняет реальные функции выбора/нормализации/risk routing
на fake MSI discovery с ASan/UBSan, 64 сочетания пар, отсутствие каскада, точный product entry,
consent boundary и source order guards. JS проходит `node --check`, resource smoke PASS.
Попытка Windows GNU crosscompile полного Unins упёрлась в отсутствующую реализацию WRL::Callback
в MinGW SDK; MSVC/WebView2 runtime этих изменений здесь не проверены. Платформа не запускалась/не останавливалась.

Из общего списка ниже закрыта реализация независимого removal Settings/предупреждения; остальные
незавершённые пункты, включая полный cleanup/profile/.theme scope и Windows matrix, остаются открытыми.

# Файловое получение пакетов подключено к движку — 2026-11-06

Рефакторинг **не завершён**. Продолжена реализация без запроса подтверждать отдельные этапы.

- Общий `GlassReleaseClient` обслуживает каталог Glass и оригинальный SecureUxTheme: WinHTTP HTTPS, ручной allowlist
  redirects, limits, bounded waits/cancel checks, streamed CryptoAPI SHA-256. Private files создаются с явным
  Administrators/SYSTEM owner/DACL, CREATE_NEW, без writers; ошибки hash сохраняются через cleanup handles.
- `GlassPackageAcquisition` проверяет реальные MSI ProductCode/UpgradeCode/ProductVersion/architecture. Current используется
  только для установленной версии/PackageCode; packages/adjacent — по trusted release digest или сертификату издателя
  текущего Setup, release downloads — по размеру и SHA-256. Неподписанный локальный MSI не становится доверенным по GUID.
- Рабочий `ExtractPayload` теперь приобретает выбранные MSI из файлов/релиза, а не извлекает их из ресурсов. Имена staged
  файлов сохранены для существующих MSI/rollback стадий. Workspace расположен под защищённым ProgramData Glass root,
  не в TEMP. Network/catalog failure не запрещает проверку независимых локальных источников.
- Cache publisher принимает файловые источники: сохраняет фактически acquired MSI, а не старые embedded payloads;
  источники неизменяемых продуктов переносятся из Current. Native/Unins resource publication пока сохранена.
- Original SUT provider использует общий клиент, проверяет packages/adjacent/Current, скачивает только в protected workspace;
  локальные файлы не перезаписываются. Оригинальный MSI не перепаковывается, existing official product по-прежнему сохраняется.
- Исправлен порядок build publication: GlassPublishLocalMsi зависит от GlassSignAfterBuild, чтобы копия рядом с EXE
  не оказалась неподписанной из-за порядка AfterTargets.

Проверено: реальные Prepare/CopyLocked на fake FS/network/inventory, реальные streamed hash guards на fake CryptoAPI,
ASan/UBSan; source/graph/installer order/cache diagnostics/catalog/SUT parser suites PASS. Engine, acquisition, client,
SUT provider скомпилированы в Windows GNU x86 objects. Это не MSVC/WiX сборка, не managed build, не WinHTTP/MSI/COM
runtime: работа реального MsiGetFileSignatureInformation и доверие сертификату требуют Windows проверки.

**Решение о локальном OpenGlass принято:** наш MSI-контейнер подписывается существующим Glass сертификатом.
DLL автора не изменяются/не переподписываются, оригинальный SecureUxTheme MSI не подписывается Glass.
Неподписанный fallback не добавлен. Build verification теперь включает OpenGlass MSI; Windows signature runtime ещё не проверен.

Лицензия: вместо старого глобального DWORD используется HKCU-отметка по ProductVersion Setup и SHA-256 обоих
переводов EULA + GPL. Modify и обычное продолжение используют одну проверку; старый DWORD, handoff и успешная
установка не создают согласие. Отметка сохраняется только нажатием «Далее» с принятием, предыдущие версии не забываются.
Проверка Windows UI/registry ещё требуется.

Существующие Settings notification/спойлер Glass/OpenGlass теперь открывают обслуживание вместо страницы download.
Никакой MSI не скачивается/не устанавливается Settings. Ошибки запуска/timeout helper обрабатываются, DLL из CWD
больше не выбирается. Это ещё не независимый product/architecture catalog и не полный Settings update scope.

Новые проверки `setup_license_mock.py` исполняют реальные identity/registry функции на fake CryptoAPI/registry
с ASan/UBSan: старые DWORD не принимают условия, версии/редакции разделены, старые согласия сохраняются,
ошибки и пустые identities закрыты, handles освобождаются. Это не проверка криптографического примитива/Windows.
`settings_maintenance_notice_mock.py` проверяет только source-контракты маршрутов/timeout/CWD/RU-EN.
Повторно PASS: release hash, acquisition, catalog sanitizer, resources, SUT graph/release, installer order,
cache diagnostics и 4096 planner cases. MSVC/WiX/.NET/реального Windows runtime здесь нет.

**Оставшийся объём всего поручения:** ресурсный inspector и удаление embedded MSI/Native; соответствующие safe fallback
и GUI/CLI consent; общая mixed-action maintenance транзакция; шесть продуктов и production UI; полная Windows проверка license routing;
предупреждение Unins Settings; отдельная cleanup page/профили/.theme; self-update handoff/backup/recovery;
простые независимые Settings update notices; миграция cache/retained и полноценная Windows matrix. Старые resource inputs
ещё нужны inspector/native preflight, поэтому их изъятие пока не выполнено. Полное поручение не считается исполненным.
Платформа/службы здесь не запускались/не останавливались.

# Лёгкий установщик: первая реализация контрактов и build output — 2026-11-06

Начат R0 и portable policy R2. Это **не подключённый файловый resolver и не завершённый refactoring**.

- `GlassPackageRules.h`: числовые версии 3/4 компонента, запрет downgrade, cache только для совпадающих installed/target,
  выбор максимальной доступной local/release версии, tie Current (только repair) → packages → adjacent → release.
  Выбор источника не разрешает запуск MSI: acquisition обязан проверить фактические identity/PackageCode/hash/architecture.
- `GlassPackageCatalog.h`: общий каталог пяти MSI (две независимые архитектуры Settings), независимые версии и ссылки
  на разные релизы. Bounded JSON parser, duplicate keys/product IDs, filenames/URLs/hash/size/types, depth/length limits;
  metadata старого формата читаются без превращения неподтверждённой ссылки в downloadable EXE. Отсутствие digest/size
  делает release source недоступным. SecureUxTheme остаётся отдельным оригинальным vendor provider.
- WiX AfterBuild публикует пять MSI рядом с EXE в `Build\GlassSetup\Release`. Исходные WiX output/staging/resource inputs
  сохранены на время перехода: новый файл-копия — build artifact, не второй runtime cache. Порядок x64 → x86 не изменён.
- `scripts/New-ReleaseCatalog.ps1`: read-only Windows Installer COM inspection версии, UpgradeCode, архитектуры,
  ProductCode/PackageCode; SHA-256/size; initial full manifest или merge отдельного MSI в прошлый каталог без потери
  записей остальных продуктов. Optional EXE entry по его ProductVersion. UTF-8 no BOM, временный файл и atomic replace.
  Не устанавливает пакеты, не подписывает и не публикует релиз. Реальные COM/MSI и PS execution требуют Windows проверки.
- В план внесены последние согласованные правила: очистка на отдельной странице полного удаления, без ссылок очистки
  на странице обслуживания; остатки только перечисляются; `.theme` учитывается; простые уведомления Settings без центра обновлений.

Проверки portable parser/rules и build graph guards PASS; resource smoke, 4096 product plan cases и original SUT parser PASS.
Полной MSVC/WiX сборки и Windows runtime проверки пока нет. Платформа/службы не запускались и не останавливались.
Следующие этапы: WinHTTP/acquisition + защищённый Current/staging, подключение к движку и removal, шесть продуктов,
production UI/cleanup/license entry, self-update handoff и единые Settings notices. В текущем runtime по-прежнему встроенные
MSI и Native, новый каталог пока не используется движком; ресурсы удаляются только при готовом replacement path.

# OpenGlass: состояние SCM и управление из Settings — 2026-11-05

- Состояние выполнения отделено от start type: SERVICE_RUNNING всегда Running, даже при SERVICE_DISABLED; Disabled только для SERVICE_STOPPED + SERVICE_DISABLED. START_PENDING/STOP_PENDING не считаются Running. При наличии службы она имеет приоритет над оставшейся legacy задачей, одинаково на UI и worker threads.
- Home и presence mapping учитывают OpenGlassOrchestration, а не только Og*TaskExists (native producer оставляет эти legacy поля нулевыми). Явные проверки установки используют orchestration API; ABI snapshot не менялся. Убрана двухсекундная отсечка RefreshSystemCard: она могла потерять единственное событие изменения; дедупликация по fingerprint осталась.
- Home/кнопка запуска System проверяют Running, не готовность DWM/symbol pipeline. Запуск отключённой службы временно использует demand start и восстанавливает disabled policy также при ошибке StartService. Остановка ждёт SERVICE_STOPPED, сохраняет регистрацию и startup policy. Native start/stop entry points используют общие реализации в существующей DLL, без новых модулей.
- System off и меню удаления идут по одному async пути. Uninstall использует lifecycle busy guard, ждёт остановки, вызывает DeleteService, закрывает собственный handle и ждёт фактического исчезновения из SCM. Уже отсутствующая служба — успешный no-op; ошибки/таймаут не скрываются, bool доходит до existing error UI. Legacy задачи также удаляются. При чужом открытом SCM handle marked-for-delete не выдаётся за завершённое удаление; ожидание ограничено 30 секундами на остановку и 30 на удаление. Принудительного закрытия чужих handles/удаления registry нет.

Проверено: `openglass_settings_mock.py` — настоящие функции SCM state/start/stop/delete и service-first query с fake Win32 API, ASan/UBSan: running+disabled, stopped+disabled/manual/auto, pending states, legacy fallback/service priority на UI/worker, сохранение startup policy, ошибки enable/start/restore/query/stop/delete, stop timeout, external handles, повторное удаление; source guards service-only wiring в C#. Четыре существующих OpenGlass state/service/stop/restore mocks, resource smoke и SUT build graph PASS. Полный PlatformPresenceNative.cpp скомпилирован в Windows GNU objects x64/x86. Компиляция полного OpenGlassHostTask.cpp ограничена недостающими Windows SDK/WIL заголовками (Psapi.h); полной MSVC/WiX/managed сборки и Windows runtime проверки здесь нет. Платформа/службы не запускались и не останавливались.

Для Windows проверки: Release|x64 → Release|x86; работающая отключённая служба — зелёная и Home on; Home off оставляет stopped/disabled регистрацию, последующий on запускает и сохраняет disabled policy; System off удаляет SCM регистрацию, on создаёт её заново. Проверить без legacy задач и при работающей/остановленной платформе. Это исправление Settings не завершает общий maintenance/SecureUxTheme backlog. Modify/maintenance UI пока не менялся: отдельная оболочка или режим существующего UI остаётся вопросом согласования.

# Исправление preflight встроенного Native — 2026-11-05

Найдена регрессия после installed-only ограничения NativeDllForArch: VerifyEmbeddedNativePayload проверял x86 из bundle, а x64 через resolver уже установленного Settings. На чистой системе или при старом установленном helper получалась ложная ошибка «встроенная DLL отсутствует/несовместима», независимо от наличия свежих Build/x64 и Build/x86 DLL и правильного порядка сборки.

Теперь обе архитектуры проверяются напрямую из RCDATA IDR_UNLOCKER_X86/X64 запущенного GlassSetup.exe, без извлечения/исполнения DLL и без install-root зависимости. Файловый и ресурсный путь используют один ограниченный по размерам PE/export parser. Installed-only resolver для SecureUxTheme не ослаблен. Проверки всё ещё идут до snapshot/остановки хостов/запуска MSI; добавлены resource ID, ожидаемая architecture, конкретный PE/export check и Win32 error в журнале вместо безусловного совета пересобрать решение.

Регрессионные тесты: настоящий preflight с mock только встроенных ресурсов (никакого установленного resolver в тесте), обе архитектуры, отсутствие/ошибочный PE/экспорты, отмена; настоящий PE parser через file/buffer/resource на x86/x64 synthetic fixtures с ASan/UBSan, включая случай «строка есть, но не экспортируется». Resource/source smoke PASS. InstallEngine и InstallUtil скомпилированы в Windows GNU x86 objects. Это не MSVC/WiX сборка и не Windows runtime проверка итогового EXE. Порядок Release|x64 → Release|x86 не изменён.

# Settings: регистрация вместо MSI install/remove — 2026-11-05

В существующем GlassSettings.Native восстановлены только операции включения/отключения IFEO/COM. Новых DLL/CA нет. Settings больше не запускает msiexec, не меняет MSI/ARP, не копирует/не удаляет системные DLL и не планирует cleanup task. Доступ к WRP registry values — scoped backup/restore privileges, без takeover owner/DACL. До записи проверяются все значения; rollback сохраняет исходные типы/данные и не затирает конкурентные чужие правки. Он in-process, не crash-durable. Включение проверяет/удерживает полный native/WOW payload; отключение активного механизма требует фактического stock style. До reboot уже загруженные хуки могут продолжать работать.

MSI, файлы, регистрация и loaded state разделены: MSI сам по себе не включает переключатель; inert DLL/proxy не дают Conflict; оставшийся COM proxy доступен для explicit off, но proxy без патчера не даёт включить механизм. Официальный MSI определяется по трём UpgradeCode, независимо от того, кто его установил. Setup сохраняет существующий официальный продукт, без ошибки «внешняя установка», скачивания или неявной повторной активации. Общий inspector не требует Glass install root для официального SUT. Отдельно блокируются активные несовместимые pre-v4 hooks, чужие COM значения и старые Glass-repacked MSI для неявного перехода.

Проверено: actual Native registration code на fake registry (on/off, native/WOW, чужие tokens/bits/COM, остаточная регистрация, ошибки на каждом из 6 шагов записи, rollback при конкурентной правке); полный SecureUxThemeNative.cpp — Windows GNU object x64/x86 с upstream WIL headers; provider/engine/inspector source smoke и portable suites. Это не MSVC/WiX, не managed build и не реальные Windows privileges/COM/reboot. Платформа не запускалась/не останавливалась.

**Незавершённое:** GlassUnins ещё не удаляет оригинальный продукт по динамическому ProductCode. Предложенный каталог ProgramData/SecureUxTheme и предварительная проверка доступности для карточки также ещё не внедрены: актуальный download пока adjacent. Не считать переработку maintenance завершённой. Матрица реального Windows runtime обязательна.

## Предыдущий этап (история)

# Оригинальные MSI SecureUxTheme — 2026-11-05

Вместо перепаковки реализованы runtime release discovery / adjacent local-first download / первичная фоновая установка оригинального MSI. См. `README.md`, раздел «Оригинальный MSI SecureUxTheme». CA, собственные SUT WiX-проекты, ресурсы и build dependencies удалены. Неиспользуемая собственная IFEO/COM-регистрация также удалена из GlassShared; Native больше не ссылается на неё. Официальные permanent proxy semantics сохраняются.

Подробности: native-architecture asset; строгий JSON parser, HTTPS/allowlisted redirects, SHA-256 из GitHub API, offline pinned v4, fail-closed без замены существующего MSI, защищённый snapshot и MSI metadata. Vendor MSI идёт последней транзакцией перед commit, без Glass INSTALLDIR/ARP/rollback product adoption. Уже установленный оригинальный продукт и старый Glass SUT требуют явного отдельного обслуживания.

Это **не завершение всего maintenance**. Settings/Unins ещё не переведены на динамическую идентичность оригинального MSI; существующая инвентаризация официального продукта требует обновления. Старый механизм установки SUT из Glass cache не считается рабочим для оригинальных пакетов. Windows/MSVC/WiX runtime здесь отсутствуют; фактические загрузка через GitHub redirects, MSI ProductCode/metadata, install/reboot/rollback должны быть проверены в VM. В песочнице GitHub asset download ограничен сетевым redirect; бинарник MSI не исследован и установка не выполнена.

Проверки текущего изменения: portable C++ parser tests (три архитектуры, offline pins, 42 invalid manifests), source/resource smoke, статический build graph без SUT CA/repack, Windows GNU object compilation provider/engine. Это не успешная MSVC/WiX сборка и не Windows runtime тест. Попытка GNU-компиляции полного main.cpp остановилась на отсутствующем восстановленном Detours header; это не тест успешной линковки EXE.

## Исторические правки до перехода на оригинальные MSI

Ниже сохранён журнал частичного обслуживания. Упоминания собственной CA относятся к старой реализации и больше не являются планом её развития.

# Правки обслуживания после аудита 233b21c — 2026-11-05

**Рабочая публикация для проверки сборки и диагностики в изолированной Windows VM. Полное исправление обслуживания ещё не завершено; это не готовый выпуск.**

В рабочем дереве исправлены: преобразование MSI errors в HRESULT для GlassSettings (положительный 1603 больше не считается успехом); перепроверка регистрации после MSI removal; машинное чтение/запись install-path в Setup и основных managed/native читателях GlassSettings; машинный snapshot путей rollback; установленный Native helper вместо извлечения в GlassUnlocker; отсутствие SUT style preflight/query при отсутствии Settings в Unins; допустимый ABSENT component в MSI path inspection; обязательный выходной буфер MsiEnumProductsEx; экспорты CA DLL; deferred вместо commit CA и явные условия REMOVE=ALL; отключены старые native entry points ручного удаления системных файлов/регистрации; убрана тавтология OpenGlass. Исправлен mock защищённого child ACL и устаревшие source assertions из предыдущей реализации.

Проверено: resource/source smoke, 4096 portable planner cases, cache diagnostics mock; Windows GNU object compilation InstallUtil/InstallMsi/InstallRollback/InstallEngine и SUT CA x86. Это не MSVC/WiX и не Windows runtime.

**До завершения остаются блокирующие части:**
- Реальный maintenance plan с явными действиями Keep/Install/Repair/Update/Remove и единым выполнением GUI/CLI; сейчас Setup по-прежнему не умеет удалять.
- Полное Settings/Unins обслуживание оригинального SUT: новый install-path использует отдельную проверку доверия и не обходит общие fixed-root ограничения для продуктов Glass.
- Автономные stop/delete собственной службы и задач OpenGlass в MSI, обработка оставшейся отключённой службы; сейчас этот сценарий ещё не исправлен.
- Собственная SUT CA отменена; IFEO/COM устанавливаются только оригинальным пакетом автора. Не реализовывать собственную замену permanent proxy semantics.
- Полный обзор остальных install metadata читателей, raw registry cleanup и ARP после частичного удаления; native fallback/UI/CLI parity.
- Безопасность исполнения кэша под общим Packages с writable-parent ACL; отсутствие reparse и protected child ACL не доказывают защиту от замены через родителя.
- Полная Release x64 -> Release x86 MSVC/WiX сборка и Windows VM matrix.

Ни платформа, ни Windows setup/uninstall не запускались. Публикация текущей ветки разрешена пользователем для сборки/диагностики; это не подтверждение завершённости. Резервные копии, временные каталоги и агентские HTML-макеты удалены; в облачной ветке для сохранения изменений используется Git.

---

# Реализация согласованной модели — рабочее состояние

Дата: 2026-11-04. **Полная переработка не завершена. Не выпускать и не проверять реальным удалением на рабочей Windows.** Готовность MSVC/WiX-сборки всего solution не подтверждена. Публикация рабочей ветки разрешена пользователем для проверки сборки; это не подтверждение завершённости.

## Подключено в исходниках

- Независимое пересечение selection/presence в `GlassProducts::NormalizeRemoval`; сняты каскады/замки UI Setup/Unins. Удаление Settings само по себе больше не отключает SUT. В engine ещё остаётся legacy package-only adoption (см. ограничения).
- `GlassProductPlan.h`: переносимый fail-closed planner. `GlassProductUiPolicy.h` использует его **только для advisory after-state/предупреждений**. Синтетические Preview Facts НЕ являются discovery и не разрешают выполнение операций.
- Встроенный `res/slideshow.html`: каталог 614 + план 274, высота 330, независимые карточки, жёлтая карточка для каждого предупреждения, ⚠ под иконкой, popup X/outside/Esc, счётчик/scroll hint, повторение предупреждений в плане. Esc внутри popup не отправляет cancel мастеру. Host-side select-all подключён в обоих мастерах; cleanup и сертификат не изменяются.
- `GlassProfileMaintenance::Inventory`: ProfileList, загруженные HKU или приватный `RegLoadAppKeyW(KEY_ALL_ACCESS)`, удержание hive handles. Отсутствующий/недоступный NTUSER.DAT блокирует preflight, а не молча пропускается.
- `RegistrySanity::CleanOwnedSettings`: проверка доступа до commit, проверяемые ошибки, очистка GlassPlatform config с сохранением четырёх install-path значений; в DWM удаляется только whitelist OpenGlass. Unins вызывает preflight до stop/MSI, cleanup в конце; поздняя ошибка отмечается как partial. Scope включает сохраняемые продукты, HKLM и локальные профили. При cleanup останавливаются/восстанавливаются затронутые retained backends, используется modules-release gate.
- `SystemVisualStyle.h`: фактический текущий стиль через GetCurrentThemeName, а не display name `.theme`/basename `.msstyles`. Stock требует точного Windows resource path без reparse и успешной offline WinVerifyTrust с Microsoft Corporation signer. Неопределённость блокирует reset/removal. После live apply штатного стиля производится повторная проверка. Hook остаётся при неуспехе.
- Консервативный SUT preflight всех профилей. Чужой активный сеанс пока не имеет достоверного session broker и классифицируется как не обследованный. Пользовательский/неизвестный offline style блокирует removal: непроверенный registry reset НЕ выполняется. Проверяется соответствие повышенного аккаунта владельцу рабочего сеанса (over-the-shoulder UAC не должен сбрасывать HKCU другого аккаунта).
- Подготовка/деактивация SUT перенесена за atomic cancellation boundary, непосредственно перед MSI removal. Повторный style preflight выполняется перед helper. Результаты reboot MSI агрегируются; GUI и silent exit сохраняют 3010.
- Query SUT использует встроенный helper. Unins встраивает только x64 helper первого прохода. x86 helper публикуется финальным Setup в защищённый Current и используется Unins на 32-битной ОС; предварительная сборка x86 Native не требуется.
- Начат `GlassMaintenanceCache.h`: защищённый `%ProgramData%\Packages\GlassPlatform`, Global mutex, Current/Staging/Previous/Retained, обе оболочки и текущие MSI; all-user MSI registration discovery, PackageCode/version matching, добавление repair source, консервативная GC при неполном discovery. Публикация вызывается после MSI commit; ошибка становится finish warning, Previous не удаляется до регистрации sources. Полная транзакционная модель ещё НЕ готова.
- Диагностика публикации кэша: этап/путь/API/код, отдельные домены Win32/MSI/HRESULT/Policy, owner SID и write ACE при отказе доверия, первая ошибка в finish warning. RU/EN заголовок и описание итоговой страницы больше не приписывают любому замечанию сбой запуска. По журналам чистой установки все MSI и запуски успешны, но причина cache failure ранее не записывалась. Подозрителен строгий ACL/owner check общего `ProgramData\Packages` до создания собственного каталога; без нового журнала/ACL это **не подтверждённая причина**. Политика доверия и ACL общего каталога не ослаблены и не переписываются.
- ARP UninstallString/QuietUninstallString тихие; ModifyPath ведёт к cached Setup GUI (fallback — interactive Unins). Для Settings при установленном SUT выставляется NoRemove; engine guard silent removal остаётся обязательным, UI-скрытие не заменяет его.

## Проверено в этой среде

- `python GlassSetup/tests/run_product_plan_tests.py`: PASS, 4096 комбинаций + focused mutation/bulk/style tests.
- `selection_tests.cpp` g++ C++17 `-Wall -Wextra -Werror -pedantic`: PASS; тесты обновлены на отсутствие каскадов.
- `python GlassSetup/tests/cache_diagnostics_mock.py`: PASS на настоящих функциях diagnostics/directory preflight и resource extraction с fake Win32 API: отказ ACL/owner общего родителя до создания child, ошибка CreateDirectory собственного каталога, успешный путь, первая ошибка при recovery, сохранение ошибки записи/отмены через CloseHandle/DeleteFile. Это не проверка ACL конкретной Windows пользователя.
- `python GlassSetup/tests/verify_resources.py`: PASS. Устаревшие source assertions mandatory locks/lower stripe/current-user-only cleanup заменены проверками нового контракта; ограничения старого SUT MSI по-прежнему явно видны в тестах.
- Playwright на **LIVE PREVIEW «Макеты GlassSetup»**, fixture `runtime-products-preview.html` использует настоящий HTML-ресурс с mock host, без системных операций: PASS Setup/Unins, 5 карточек, 4 fresh defaults, 330px, host messages, warning/Esc, select-all без cleanup, нет JS exceptions. Тест `tests/runtime_ui_smoke.mjs`; external playwright-core path задаётся `GLASS_PREVIEW_NODE_MODULES`, URL — `GLASS_PREVIEW_URL`. В Arena применяется `GLASS_PREVIEW_SPARTICUZ=1`. Fixture использует preview mapping `runtime/` на `res/`. Дополнительно PASS 6 RU/EN completion-сценариев: cache note, startup note и чистое завершение; проверены нейтральные title/body, видимость подробностей и отсутствие выхода текста за 1000×540. Пример: `?page=complete&locale=ru&note=cache`; данные отказа в макете демонстрационные.
- Zig 0.17 / Windows GNU cross-compilation: новые cache/profile/UI-policy headers компилируются в x86/x64; `InstallEngine.cpp` с GLASS_EMBED_MSI, `GlassInstallMsi.cpp`, `GlassInstallUtil.cpp` компилируются для x86. Обнаружена и исправлена реальная ошибка DWORD/UINT аргумента MsiSummaryInfoGetPropertyW.
- Cross-compilation полных main.cpp остановилась на недостающем Microsoft WRL Callback в GNU SDK; это НЕ MSVC-проверка и не доказательство ошибки WRL кода проекта.
- UTF-8 без BOM, XML проектов/filters, git diff --check: PASS.
- Windows binaries/MSI не запускались. Платформа не запускалась/не останавливалась. MSVC, WiX, Windows runtime здесь отсутствуют.

## Обязательное завершение перед передачей полного проекта

1. Реальный discovery adapter + Build/revalidation в execution, общий для GUI/CLI. Сейчас Preview не решает эту задачу.
2. **SecureUxTheme:** завершить explicit maintenance оригинального MSI и обновить инвентаризацию в Settings/Unins. Системными ресурсами владеет только пакет автора, visual style — установленный helper. Миграция со старых Glass ProductCodes — только явным безопасным планом.
3. Session style broker для подтверждения фактического stock style чужого активного пользователя без ожидания logout. Проверенный targeted offline theme reset + policy assessment + post-reset checks. Сейчас такие случаи безопасно блокируются, но не реализованы полностью.
4. Cache: durable manifest/journal и recovery/reboot/rollback pins; точная source identity с SHA-256, обработка same-version/same-PackageCode, гарантия наличия source каждого kept продукта, миграция отсутствующих legacy sources, atomic source/manifest commit и удаление устаревших owned source-list ссылок. Требуется закрыть все crash windows, не выдавать текущий header за законченный cache controller.
5. ARP после частичного removal должен перепроверять remaining registrations и NoRemove. Native fallback должен соответствовать реальному HTML, локализация старых SUT dependency descriptions требует обновления вместе с MSI.
6. MSVC/WiX полная сборка из solution и Windows VM matrix: install/repair/upgrade/downgrade, совместное/раздельное удаление, silent guards, late partial errors, загрузка offline hives, active/session styles, reboot, rollback, cache corruption/recovery, отсутствие лишних sources после последующего обновления retained продукта.

Пользователь просил полный проект для сборки/тестирования и не может принимать промежуточные этапы. Этот документ — техническое состояние незавершённой работы, НЕ запрос acceptance и НЕ объявление готовности.

## UI review и публикация ветки (2026-11-04)

- Общий RU/EN STRINGTABLE ресурс `IDS_COMP_SHELL_SUFFIX`: « и оболочки Windows» / « and the Windows shell», включён в оба EXE через `ProductStrings.rcinc`.
- Описание основной платформы и предупреждение дополняются suffix только для native architecture: x64 на OS x64, x86 на OS x86. Названия карточек — Glass Platform x64/x86.
- Описание SecureUxTheme: «Средство для использования неподписанных визуальных стилей. Установка в System32.» Дублирование ответственности из описания макета удалено; предупреждение остаётся отдельным.
- Planner помечает OpenGlass предупреждением ManualOpenGlass, если продукт будет использоваться без GlassSettings. В плане, карточке и popup указано ручное редактирование реестра/управление службами и задачами; принудительная зависимость не вводится.
- Макеты и runtime fixture обновлены для обеих архитектур. UI smoke покрывает четыре случая OS x64/x86 × Setup/Unins.
- Порядок сборки неизменен: **Release|x64 → Release|x86**, без предварительной сборки проектов. Ошибочное требование x86 Native в ресурсах Unins удалено. Для 32-битного обслуживания финальный Setup публикует свой встроенный x86 helper в защищённый Current; это не добавляет обратной build dependency первого прохода на второй. Полная Windows-сборка в этой среде не проверялась.
