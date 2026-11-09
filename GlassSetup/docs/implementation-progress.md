# Текущий этап: пустые панели, Settings-only без Launching, ярлык в GlassUnins

Пользователь подтвердил успешную Windows-сборку **предыдущей** ревизии и работу
проверки нарушения целостности. Новые изменения ниже Windows-сборкой/реальным
MSI-удалением пока не подтверждены.

## Реализовано

- В пустых Available/Installed рамка растягивается на всю usable list plane с
  отступами 6px по вертикали / 10px по горизонтали. Маска скролла отключается,
  чтобы не обрезать нижнюю границу. Empty state плана занимает всю свободную
  область своего списка; надписи центрированы. Исправлена специфичность CSS,
  иначе общий `li { justify-content:space-between }` перебивал центрирование.
- Checking copy RU/EN сокращён до «Ищем обновление GlassSetup.» / «Looking for a
  GlassSetup update.» в HTML и native fallback; убрано описание стадий скачивания.
- Launch-page policy зависит только от явно выбранных Platform/OpenGlass с
  действующим startup intent. Settings-only и vendor-only не открывают Launching
  и не получают искусственные пять секунд, независимо от prior host state.
  Восстановление временно остановленного **ранее работавшего** retained host
  остаётся служебной финализацией: новые невыбранные host не стартуют, прежний
  работающий host не оставляется выключенным ради сокрытия страницы.
- Частичный Settings Remove в standalone GlassUnins теперь подключён к общему
  locked-link retarget механизму: дополнительный MSI/context/cache/root baseline
  сохранённой (либо ожидаемо отсутствующей) архитектуры, private serialization до
  остановок, commit после закрытия отмены до MSI, повторная проверка baseline.
  Финализатор охватывает обычные выходы/исключения: до удаления выбранной Settings
  восстанавливает исходный link; после фактического снятия MSI не возвращает link
  на удалённый EXE даже при последующем отказе ARP/cleanup. Ошибка restore/recheck
  сохраняет защищённый recovery workspace и сообщает его путь.
- Протокол и Windows acceptance: [settings-shortcut-maintenance.md](settings-shortcut-maintenance.md).
  Историческое ограничение «только Setup» в разделах ниже заменено этим этапом.
- В постоянном preview добавлены «Пустые панели» и «Проверка версии Setup».

## Проверки

- Полный прогон: **82 Python scripts / 72 PASS / те же 10 исторических FAIL**,
  новых отказов нет: `.cache/settings-unins-final/summary.json`.
- 3 production DOM-suite PASS. Chromium smoke PASS, в том числе реальные bounding
  rectangles/insets пустых Available/Installed/plan, центрирование, отсутствие
  fade-mask на пустом списке и короткий RU/EN checking copy. Прежние update-green,
  damage-red, popup/Escape/focus и bulk-plan/undo регрессии тоже PASS.
- Реальные production policy/Tick: Settings-only policy=false; commit gate может
  быть закрыт, UI остаётся Installing, затем сразу Complete без launch timer.
- Новый `unins_shortcut_retarget_mock.py`: настоящие commit/finalizer блоки Unins
  с fake MSI/link API. `shortcut_retarget_mock.py`: настоящий stream/restore/RAII
  механизм с fake file APIs. Это не COM/ACL/locking и не Windows Installer тест.

## Не закрыто этим этапом

- Durable journal/автоматическое crash/reboot recovery; это не полная atomic
  транзакция MSI+shortcut. GlassUnins не переустанавливает уже удалённые MSI.
- Безопасный post-exit cache cleanup/self-renewal handoff; публикация Apply из
  исполняемого cache всё ещё может встретить занятый Previous.
- Общая vendor/managed atomicity и force-clean+Remove, fully per-file Platform
  snapshot, точный shell rollback, full native/C parity, standalone SUT migration,
  оставшиеся profile notices и unknown-root residual recovery.
- Новая Windows VM-проверка обоих направлений Settings retarget, нестандартных
  параметров/ACL/ADS ярлыка, отказа MSI/ARP/link write и Settings-only UI требуется.

---

# Текущий этап: удаление без ложного запуска, план и предупреждения карточек

## Диагноз по пользовательскому `GlassSetup-6.log`

Все четыре MSI удаления завершились `exit=0`. Setup был запущен из
`C:\ProgramData\Packages\GlassPlatform\Current\GlassSetup.exe`. После удаления
он ненужно опубликовал новый bundle: переименовал Current в Previous, активировал
новый Current, затем получил `Win32=5` при удалении `Previous\GlassSetup.exe`.
Это ошибка финализации cache, не ошибка MSI удаления. Путь указывает на
перемещённый исполняемый controller; отдельный Windows handle/ACL-тест здесь
не выполнялся, любые AccessDenied не объявляются безусловно безопасными.

## Изменения

- При отсутствии Package Apply финализация не публикует/не вращает bundle.
  Существующий cache и источники оставшихся продуктов сохраняются, ARP refresh
  выполняется по-прежнему. Реальные ошибки ARP и публикации при Apply не скрываются;
  ACL/reparse/trusted-directory проверки cache не ослаблены.
- Отдельный atomic `hostLaunchExpected_` управляет только страницей запуска и её
  минимальной длительностью. Старый commit gate `launchingPhase_` по-прежнему
  закрывает отмену под status lock, даже когда launch page не нужна. Полное
  удаление → Complete без Launching и искусственных пяти секунд. При частичном
  обслуживании ранее работавший сохранённый host может требовать перезапуска.
- «Удалить всё» в панели плана переиспользует поштучный IPC Remove, снимает Apply
  intents и планирует поддерживаемые удаления. Установленные locked/disabled/
  unsupported продукты не обходят прежние gates, очистка данных не выбирается.
  В Unins используется существующий `components:select-all`, тоже только план.
- Предупреждения убраны из списка плана; popup доступен через жёлтый SVG-треугольник
  в правом верхнем углу основной и зеркальной карточек, с Escape/возвратом фокуса.
- Available update-карточки зелёные также в idle и hover: исправлена специфичность
  относительно legacy/idle/has-warning CSS. Повреждение остаётся красным, без
  автоматического Apply.
- Постоянный каталог и read-only preview «Макеты GlassSetup»: настоящий HTML с
  mock host, существующие концепции, предоставленный PNG-образец. Инструкция:
  [mockups/README.md](mockups/README.md).

## Проверки и границы

- Все **81** Python-тест: **71 PASS / 10 прежних FAIL**, новых отказов нет.
  Полный отчёт текущего прогона: `.cache/removal-ui-final/summary.json`.
- 3 production DOM-suite PASS; Chromium smoke PASS: RU/EN, Setup/Unins,
  bulk-plan/undo, triangle popup/Escape/focus, green idle/hover, red damage,
  отсутствие предупреждений в плане, финальная страница, исходный образец.
- `removal_completion_mock.py` исполняет настоящий maintenance prefix с fake
  cache/ARP API; `setup_real_entry_mock.py` исполняет настоящий GUI Tick и проверяет
  прямой Complete без launch timer; cancel-gate regression подтверждает запрет
  отмены после commit без привязки к наличию launch page.
- **Windows/MSVC/WiX сборка и новое реальное MSI-удаление не выполнялись.**
- Это не полная очистка cache после выхода работающего controller: безопасный
  post-exit cleanup/handoff ещё нужен. Публикация при Apply из исполняемого cache
  может по-прежнему упереться в занятый Previous; здесь такие ошибки не маскируются.
- Прежний запрет совместного managed Remove и original vendor SUT в одной
  транзакции сохранён: bulk-план не делает эту комбинацию исполнимой. Durable
  journal, standalone shortcut retarget и остальные ограничения ниже не закрыты.

---

# Текущий этап: версии MSI и переназначение общего ярлыка Settings

Пользователь подтвердил сборку **предыдущего** состояния. Изменения этого этапа
ещё НЕ проверены Windows/MSVC/WiX сборкой или исполнением.

## Реализовано в этом этапе

- Installed/Available используют версии отдельных MSI: registration VersionString,
  read-only Property.ProductVersion проверенного локального MSI либо metadata
  конкретного release asset. OpenGlass.dll больше не подменяет MSI baseline.
- Общий read-only discovery для Checking и acquisition: отсутствующие продукты
  обследуются тоже, без workspace, копирования/скачивания MSI и cache publication.
- Сравнение числовое, включая major=0; ошибочные версии не превращаются в нули.
  Удалён глобальный preflight downgrade по Setup EXE vs максимум PE версий платформы.
  Корни и per-product downgrade/context/baseline guards сохранены. В UI Installed
  показывает текущий MSI, Available — целевой; повреждение остаётся красным при
  одновременном наличии обновления, Apply не выбирается автоматически.
- Пять независимых MSI version overrides в `msi/ProductVersions.props`; пустые
  сохраняют старую bundle base/build политику. Оригинальный vendor SUT не перепаковывается.
  Его неизвестная доступная MSI версия не угадывается по release tag. Контракт и
  пределы 3/4-польного versioning: [msi-product-versions.md](msi-product-versions.md).
- Setup partial Settings Remove переназначает существующий owned общий .lnk на
  оставшуюся/устанавливаемую архитектуру. Чужая цель отвергается до MSI. Уже верная
  цель/отсутствующий ярлык не заменяются. Сериализация меняет target и только старые
  стандартные working-dir/icon ссылки, сохраняя custom параметры, description,
  hotkey/show-mode. Оригинальный файл удерживается от writers/deleters; commit
  перепроверяет bytes/security и наличие безопасного retained EXE. В отличие от
  delete/create, изменяется только default stream того же объекта: ACL/owner/ADS
  не заменяются. Откат через удерживаемый handle возвращает backup bytes и basic
  metadata, включая случаи short/failed write, truncate/flush failure. Изменившаяся
  security не перезаписывается; при ошибке остаётся rollback failure и backup.
  Это **не** crash-safe journal: авария процесса посередине записи ещё не покрыта.
- Portable проверки: **80 Python / 70 PASS / те же 10 исторических FAIL**; три
  production DOM suites PASS. Добавлены actual MSI reader/Unins projection,
  пять разных локальных ProductVersion без downloads/workspace, source-loop
  inspection и Installed/Available DOM с отличающейся PE версией, locked-link
  stream commit/rollback и real Session move/reset ownership tests. Это API seams
  и source/build wiring, не Windows locks/COM/MSI/ACL или исполнение MSBuild/WiX.

## Остаётся

- Общий vendor/managed lifecycle, force-clean+Remove, durable recovery/journal,
  self-update handoff, полный пофайловый Platform snapshot и exact shell rollback.
- Полный native/C parity, standalone original SUT migration, завершение профильной
  очистки/уведомлений, unknown-root residual recovery.
- Shared shortcut retarget подключён **только к Setup**; standalone Unins ещё
  не переведён на этот reversible session protocol. Нельзя объявлять весь пункт
  про ярлык завершённым или обещать восстановление после crash/reboot.
- Windows acceptance: разные реальные MSI ProductVersion, offline fresh/maintenance,
  kept product новее Setup, отказ выбранному downgrade; partial Settings Remove
  обоих направлений, custom shortcut, cancelled Apply и commit/rollback failures.

---

# Повторный аудит C: возврат при первичной установке и кнопки — 2026-11-07

## 2026-11-07 — явное удаление неактивного оригинального SUT из Setup

Пользователь подтвердил сборку предыдущего discovery/Build этапа. Новые изменения
ниже ещё НЕ подтверждены Windows/MSVC/WiX/runtime проверкой.

- Добавлен отдельный explicit `secureUxThemeRemove` intent. Production HTML отправляет
  `component:secureUxTheme:remove`, native receiver принимает его только после
  inspection, для единственного original vendor ProductCode и состояния Inactive.
  ProductCode фиксируется на native стороне, не принимается из WebView. `:0`
  отменяет удаление; Select All его не создаёт и не подменяет на установку.
- Для этого capability в C-карточке появилась настоящая корзина. Drag idle installed
  SUT в Available добавляет Remove; pending update/install по-прежнему сначала
  отменяется. Как в C, committed карточка остаётся на Installed с «Будет удалено» до
  Apply, каталог/«нужно» и план отражают removal. Restore/крестик отменяют его через
  native IPC, без локального скрытия продукта. Added RU/EN Apply caption, обновляемый
  при выборе/отмене. GDI fallback также предлагает явно подписанный «удалить MSI»
  checkbox, а не выдаёт удаление за снятие install-отметки.
- Unsupported repair/upgrade уже зарегистрированного SUT больше не выглядит рабочей
  операцией: native `applySupported=false`, disabled repair, no automatic selection,
  запрет direct checkbox/IPC и Select All. Установка отсутствующего SUT остаётся opt-in.
- `PrepareRemoval` требует единственный machine original MSI, отсутствие старого
  Glass repack, безопасные contexts, protected Windows Installer LocalPackage без
  reparse/hardlinks. Проверяются vendor name/family/native architecture, точный
  ProductCode и registered PackageCode; private baseline фиксируется SHA-256.
  `RecheckRemoval` перед MSI повторяет inventory/context/identity и сравнивает оба
  private snapshots с pinned digest. Никакие System32-файлы вручную не удаляются.
- Vendor uninstall — одна последняя MSI-транзакция с закрытой UI cancellation,
  после всех fallible Glass registration шагов. При отказе до неё vendor не
  затрагивается, ранее изменённые Glass продукты идут по существующему rollback.
  У vendor MSI собственная MSI-транзакция: это НЕ общий persistent atomic rollback
  Glass+vendor, и cached baseline НЕ выдаётся за полноценный reinstall source.
- Если выбран только SUT Remove, actual worker НЕ закрывает Settings, НЕ делает
  Glass snapshot/stop/scan/MSI/purge/registration/cache publication и НЕ запускает
  хосты после commit. Execution не скачивает каталог/пакеты: создаётся только
  private workspace и baseline из Windows Installer cache. Если есть выбранные
  Glass изменения, сохранён обычный lifecycle и ранний vendor preflight до stop.
- Actual-source C++ fake tests: `secure_ux_removal_mock.py`,
  `secure_ux_remove_worker_mock.py`, `secure_ux_remove_ipc_mock.py`,
  `vendor_only_stages_mock.py` PASS. Покрыты singleton/machine/context/cache identity,
  PackageCode/bytes tampering, active/pending/unknown rejection, consent binding,
  cancellation, vendor errors/reboot, no-stop vendor-only и прежний mixed path.
  Production DOM/Chromium PASS: explicit Remove drag, native echo, two controls,
  no fake repair, plan/cancel/keep, files-only pending-install cancellation и
  прежние C geometry/gesture/panel checks.
- Финальный полный Python-прогон: **53 PASS / 11 прежних baseline FAIL** (те же
  module/task/path/watch suites, перечисленные в предыдущей записи). DOM и Chromium
  повторно PASS на итоговом production HTML; `git diff --check` PASS.
- Границы: только один original vendor MSI и подтверждённый Inactive. Active,
  pending, unknown/no compatible helper, legacy repack, ambiguous/foreign inventory
  и неподтверждённый cache остаются заблокированы. General Glass mixed Remove,
  original SUT repair/upgrade, active-style/profile workflow и перенос original
  vendor удаления в standalone Unins ещё не завершены. Полного C parity/общей
  транзакции/persistent recovery/self-update handoff эта запись не заявляет.


## 2026-11-07 — реальный discovery → Build в worker Setup (продолжение п.2)

- После acquisition и rollback snapshot, но ДО закрытия Settings/остановки хостов,
  worker вызывает `BuildExecutionPlan`. Ручное присваивание Install трём строкам
  в `RunMsiSequence` удалено. План строится через `GlassProductPlan::Build` для
  Platform x64/x86, Settings x64/x86 и OpenGlass из свежего installed inspection,
  состояний MSI, защищённых staged MSI и проверенных rollback baselines.
- Проверяются точный ProductCode подготовленного MSI, численная версия каждого
  modern/legacy installed baseline (downgrade запрещён по любому из них), контексты,
  root/cache identity recheck и related products. Изменившаяся presence, неизвестная
  целостность выбранного installed продукта, отсутствующий baseline/media и
  невалидный план блокируют выполнение до остановки/изменений продуктов.
  Unchecked/kept строки не получают mutation authority и не требуют media.
- Обе Settings-разрядности теперь исполняются общей Helpers-фазой того же плана;
  сохраняются независимые early-attempt rollback bits и остановка после первого
  отказа. Products-фаза сопоставляет callback с Product, а не индексом вектора.
  Неожиданный Remove явно отвергается, а не превращается в установку.
- Добавлен `Inputs::inspectSources=false` для повторного execution inspection:
  не загружает каталог/MSI повторно и не создаёт acquisition workspace. Обычный
  UI inspection сохраняет source discovery. Исправлена проверка legacy integrity:
  baseline/компоненты берутся по реально установленному legacy ProductCode,
  а не отсутствующему modern code.
- Новые `setup_execution_plan_mock.py` и `inspection_preflight_mock.py` компилируют
  и исполняют actual adapter / actual inspector loop с fake API; PASS. Расширен
  actual early-Settings тест: нет/невалидный план, обе архитектуры, attempt masks,
  отказ первого MSI; ASan/UBSan PASS. Planner 16384/4096, fixed-path, acquisition,
  Setup download, source/resources и DOM PASS.
- Полный Python-прогон: 49 PASS / 11 FAIL после обновления source assertion на
  корректный installedCode. Имена оставшихся 11 совпадают с ранее зафиксированным
  baseline: module_scan, platform_mutex, preserve_registry, registered_paths,
  scan_protocol, shell_install_root, task_close_cleanup, task_scheduler_stop,
  unins_close_state, unins_task_stop, watch_lifecycle (суффикс `_mock.py`).
- Это НЕ завершение п.2 или mixed Remove: original SUT сохраняет отдельный последний
  vendor transaction/commit protocol; его repair/remove не авторизованы новым планом.
  GUI/worker version-intent parity, единая транзакция Setup/Unins и persistent
  recovery/removal rollback остаются в работе. Полная Windows/MSVC/WiX проверка
  текущих изменений не выполнялась; платформа/службы не запускались/останавливались.


## 2026-11-07 — SecureUxTheme residual-files investigation

- Traced the actual Native `GlassSettings_SecureUxThemeQueryStateV1W` export:
  inert System32 v4/proxy files yield Inactive; a legacy DLL alone does not
  yield External. Active legacy IFEO tokens and failed queries retain their guards.
- Found and fixed an independent native presence-classification gap in shared
  `SecureUxThemeInventory::Query`: related-product enumeration includes advertised
  products and stale family references, not just installed packages. UNKNOWN and
  ADVERTISED now do not mark a product installed; duplicate ProductCodes are
  deduplicated. DEFAULT remains installed; ABSENT remains conservatively present
  because it can represent a different user's installation. Broken configuration
  returns an error (the installation executor rejects inventory errors).
- Added portable tests compiling the ACTUAL shared inventory and Native query-state
  export with fake MSI/observations, not replacement policy implementations:
  `secure_ux_inventory_mock.py` and `secure_ux_state_mock.py` PASS. Actual Native
  registration fake-registry regression also PASS.
- Added Chromium SUT-specific files-only scenario: selected pending installation
  drags by title to Available, emits exactly `component:secureUxTheme:0`, and stays
  there after native echo. DOM, Chromium and resource/source checks PASS.
- Scope/remaining limitation: this fixes stale/advertised MSI presence, not removal
  of a genuinely registered inactive MSI. Such a product still belongs to Installed;
  returning it requires the real mixed Remove operation still outstanding below.
  No local-only shelving/hiding, falsified installed flag, MSI deletion, or file
  cleanup was added. User's Windows case is not yet reproduced on Windows and this
  change is not claimed as confirmation that their exact machine is fixed.
  Residual author-owned files are allowed and are never deleted by this change.


## Воспроизведённый дефект и исправление

Новый реальный Chromium test с fresh defaults (5 выбранных продуктов, SUT opt-in) **сначала
упал**: движение мышью за название давало 0 ghost вместо 1. Название было native button,
а весь `button` исключался из drag; прежние тесты брали карточку за строку версии и это упустили.
Название теперь span/drag surface как в C, с role=button и Enter/Space/click для сведений.
Завершение/отмена drag подавляет порождённый click, в том числе отложенный после Escape;
отдельный keyboard click не подавляется. Ghost исключён из tab order.

Fresh card действительно возвращается из Installed в Available: один `component:<id>:0`,
исчезновение из центральной полки, обновление плана/«нужно», сохранение результата после native
state echo. Обратная стрелка добавляет её снова, корзина отменяет pending install. Это не MSI
uninstall и не запрещённый mixed Remove. Другие разрядности Settings остаются выбранными.

## Упущенные отличия карточек

- Кнопки теперь имеют явные C-действия/подписи: «Установить <продукт>», «Обновить до <версия>»,
  «Переустановить <версия>», «Отменить обновление», «Отменить переустановку», «Удалить» для pending
  install; direct Unins — «Оставить установленным». Убрано общее «Отменить: ...».
- У pending install корзина danger без pressed/is-on. Каталоговая стрелка **не наследует danger**
  при клонировании pending card. Размер glyph корзины 12 px, стрелки 11 px, как в C.
- Версии без лишнего отступа и flex-gap; название, включая x64/x86, одного кегля 11 px, без
  несогласованного уменьшенного architecture tag. Цвета иконок purple/cyan/amber перенесены из C.
  Исключение для предупреждения под иконкой сохранено по прямому требованию пользователя:
  на такой карточке metadata отступает, чтобы не налезать на ⚠.
- Центральная полка называется «Установлено», не «Установлено / план». Удалён старый параллельный
  пересчёт полок из updateComponentsStatus: представление/счётчики контролирует один shelf renderer.
- Fresh defaults больше не описываются как обязательная установка в info reasons. Security/OS/SUT
  conflict gates не ослаблены. В SUT JSON добавлены уже известные verified target/package version
  и damage facts; непроверенная версия по-прежнему не выдумывается.

## Проверки и оставшиеся расхождения

PASS: реальный свежий сценарий pointer-by-title → Available → native echo → повторный выбор/корзина;
Escape с задержанным mouse-up, keyboard info; прежние drag/panel/scale/IPC tests; DOM state/action
matrix; resource smoke, portable planner/executor и acquisition/license mocks.
Дополнительно Chromium **сравнивает production с самим C** в состояниях available / pending install /
selected catalogue: текст meta/source/chip, title/aria, SVG, количество кнопок, их размеры,
цвет иконки, кегль и metadata alignment. Это больше не только проверка внешнего прямоугольника.

Полный паритет всех maintenance состояний пока НЕ заявлен. В частности, в C у реально
установленного продукта две кнопки (↻ и удаление), а mixed Remove в Setup всё ещё не реализован;
фиктивная работающая корзина не добавлена. Самопродление/handoff и п.2/3 остаются незавершёнными.
MSVC/WiX/Windows WebView runtime здесь не запускались. Платформа не запускалась/не останавливалась.

# Карточки C / фильтры и проверка нового Setup — 2026-11-07

## Исправлено по повторному UI review

- Карточки двух полок больше не имеют одинакового контента. «Доступно» — каталог релиза:
  version/size и маркеры «локально / на сервере», без installed/action chip. Выбранное действие
  приглушает каталоговую карточку; повторная стрелка не отменяет его. «Установлено» — текущий
  состав: текущая версия, предложение новой версии «в Доступно», planned install, update,
  reinstall, damage и (в direct Unins) removal. Update/reinstall в плане зелёные, кнопка ↻ нажата.
  SVG glyphs действий/статусов взяты из C; сохранены 72 px, skins, warning под иконкой, info popup.
- «Доступно / нужно»: отсутствующие невыбранные, непланированное обновление или восстановление;
  уже выбранные скрыты. «Установлено / нужно»: внимание или действие в плане. Само предупреждение
  о ручной настройке не делает исправный продукт требующим установки/переустановки.
  «Все» сохраняет весь каталог. Footer/count/объёмы и empty states пересчитываются по видимым
  карточкам. Размер показывается только при известных данных, серверный источник не выдумывается
  из наличия соседнего MSI. Registry/unknown failure не подписывается как SHA-256 mismatch.
- Удалён старый refresh handler: после pointer drop он перезаписывал новый ↻ старым X.
  Intent по-прежнему один, Settings x64/x86 независимы. **Mixed Remove в Setup не реализован**:
  не добавлена фиктивная работающая корзина. Поэтому полный UI/backend parity ещё не заявлен.

## П.6: рабочая стадия Checking, но ещё не завершённое самопродление

- Legacy `Page::Update`, отдельный HTML panel и маршрут Download/Skip удалены из кода мастера
  (включая debug tour). Проверка получает bounded catalogue через существующий ReleaseClient;
  версии/размеры продуктов из него поступают на страницу обслуживания, а не отдельное предложение.
  Maintenance пропускает Welcome, но тоже проходит Checking перед лицензией/продуктами;
  `--no-self-update` запрещает загрузку Setup, не отключая discovery версий продуктов.
- Новейший downloadable setupPackage скачивается **на Checking**, с отдельным статусом в WebView
  и GDI fallback. Используется private Work staging, CREATE_NEW .part, размер/SHA-256,
  strict WinVerifyTrust + совпадение signer с текущим Setup, PE ProductVersion == manifest.
  Cancel закрытия передаётся worker до join; временный workspace удаляется при завершении мастера.
- **Publication/handoff ещё НЕ подключены.** Проверенный EXE не запускается: текущая версия
  продолжает мастер, в логе это явно указано. Durable replacement/recovery, unlimited user .bak,
  cached EXE/HKLM rebinding и аргументы/мьютекс при перезапуске остаются обязательным продолжением.
  Лицензия не принимается автоматически. Не выдавать скачивание за завершение всего п.6.

## Проверки

PASS: expanded production jsdom state/content/filter matrix; actual Chromium geometry/skins C
и pointer/panel regression; production PrepareSetup failure injection с ASan/UBSan (fake Win32,
не реальный WinTrust); package acquisition и version-scoped EULA mocks; portable planner/executor;
package catalog/output guards; resource smoke. Windows GNU x86 object compilation обновлённого
GlassPackageAcquisition.cpp — PASS. MSVC/WiX, сеть Windows/сертификаты/реальный EXE handoff не
проверялись. Платформа и Windows Setup/Unins не запускались. П.2/3 также остаются незавершёнными.

# Исправление drag/panels C и прямого MSI output — 2026-11-06

## Production UI, не mock handlers

- Карточки действительно перетаскиваются мышью: threshold 5 px, исходная карточка затеняется,
  ghost сохраняет grip, размеры и SVG/skin C, цель подсвечивается с анимированным veil.
  Ghost живёт внутри production panel, координаты переводятся из client в stage units;
  fit/DPI scaling не масштабирует его дважды. Кнопки/имя/⚠ не становятся drag handles.
- Drop использует единственный настоящий checkbox и существующий `component:<id>:0|1` IPC,
  включая зеркала каталога. Setup: вперёд — выбрать install/update/repair, назад — только
  отменить уже выбранное действие. **Remove сохранённого продукта в Setup не имитируется:**
  отказ показывается в цели/кратком сообщении, IPC не отправляется. В direct Unins используется
  его существующий реальный remove intent; здесь не заявлено выполнение Windows removal.
- Escape/outside drop/blur/pointercancel/lost capture/смена страницы или discovery facts
  отменяют жест без изменения плана. Неизменённый native tick не пересоздаёт source alias
  посреди drag. Disabled/locked/loading gates сохраняются; ghost не содержит inputs/IDs.
- Обе крайние панели имеют настоящие pin buttons. Незакреплённая панель плавно сворачивается
  после действий (1 s), минимум одна крайняя панель остаётся видимой. Центральная полка
  расширяется как в C. Скрытая панель inert/aria-hidden; возвращается кнопкой края/Enter
  либо hover 2 s, в том числе **во время drag**; уход от края отменяет таймер. Закрепление
  исключает автоскрытие. Фокус не оставляется внутри скрываемой панели. Учитывается
  prefers-reduced-motion; таймеры/drag очищаются при уходе, blur и открытии info/warning.
- Обе панели первоначально видимы; кнопки, фильтры, выбор всех, popup и независимые
  Settings x64/x86 остаются production controls, не демонстрационной моделью C.

## MSI: конечный выход, а не публикационная копия

- И ранний `GlassSetup/msi/Directory.Build.props`, и поздний override `Directory.Build.targets`
  задают настоящий OutputPath `Build\GlassSetup\$(Configuration)\`. Для Release все пять
  уникальных MSI лежат непосредственно рядом с Setup. OutDir/TargetDir/TargetPath совпадают;
  `GlassPublishLocalMsi` и его post-build Copy удалены. Подпись работает с конечным TargetPath.
- BaseOutputPath и obj/tracking остаются project-private в Cache; foreign-platform
  Build/Clean/Rebuild guard сохранён. Обычные внутренние Cache intermediates WiX SDK
  не являются вторым release output. Payload staging и проверки построенного MSI сохранены.
- Portable package/ASan/UBSan, XML output/signing/source guards, planner/executor tests,
  resource smoke, jsdom и real Chromium production/reference tests: PASS.
  Browser regression выполняет реальные pointer gestures, pins/hover/keyboard, проверяет
  geometry/skins C, единственность IPC, cancellation и scale 0.8. Screenshots — внешние
  fixture artifacts, в Git не добавляются. **MSVC/WiX Build/Clean/Rebuild/signing на Windows
  здесь не выполнялись**; сохранность соседних outputs при реальном Clean ещё нужна в VM.

Это исправление двух замечаний, не завершение всего п.2/п.3. Общий mixed Remove в Setup,
полный rollback Unins и Current-only migration остаются незавершёнными; подробности ниже.
Платформа не запускалась/не останавливалась, MSI/Setup/Unins не исполнялись.

# Возврат к утверждённому варианту C; продолжение п.2/п.3 — 2026-11-06

Исправление после замечания пользователя: одноколоночная страница d6e2187 была отступлением
от утверждённого макета, а не его production-реализацией. Её собственная компоновка убрана.

## Интерфейс

- В production `res/slideshow.html` перенесены shelf/card/plan skins именно из варианта C:
  слева каталог 302 px (x=48), установленный/запланированный состав 302 px (x=366), справа
  план 268 px (x=684). Все три блока y=156, высота 372; карточки 72 px. Заголовок y=65,
  шрифт 27 px; фон 820×540 с reference mask; логотип и SVG продуктов соответствуют макету.
- «Доступно» показывает весь каталог, не только отсутствующие продукты. Зеркала установленных
  карточек не создают второй checkbox/intent: один authoritative control на продукт и отдельные
  Settings x64/x86 IPC. «Все / нужно», выбор из обеих полок и отмена строки плана работают.
  Установленное и запланированное различаются dashed-карточками, подписью/счётчиком в footer.
- Описание/причины скрыты в карточке, но доступны по имени продукта. ⚠ остаётся под иконкой;
  предупреждение повторяется в плане. Info/warning popup закрывается X/outside/Escape, без
  нижних кнопок. Никакие mock handlers/демонстрационные install/remove операции не перенесены.
- Select all находится в плане и не включает cleanup/cert. План сразу показывает выбранное
  действие (а не устаревшую server-state подпись), имеет реальную отмену через тот же IPC.

**Визуальная проверка теперь есть:** внешний Chromium 153 установлен через npm в cache, без
новой зависимости приложения. `maintenance_ui_browser_test.cjs` открывает production HTML
и исходный утверждённый HTML, сравнивает реальные rectangles и CSS skins блоков/карточек,
проверяет Settings IPC/перенос и info/Escape. Есть реальные screenshots обоих HTML.
Это Linux Chromium + fixture discovery, **не Windows WebView/native footer** и не утверждение
полного pixel parity. Pin/auto-collapse/drag UI подключены в исправлении ниже; mixed Remove
в Setup по-прежнему не включён до работающего Windows backend. Не объявлять весь UI готовым.

Запуск browser regression с внешним браузером:
`NODE_PATH=<external playwright-core modules> CHROMIUM_PATH=<chrome> node GlassSetup/tests/maintenance_ui_browser_test.cjs`.
Для packaged Chromium использовался внешний `@sparticuz/chromium`, его bundled al2023 libs
в LD_LIBRARY_PATH. `GLASS_UI_SCREENSHOT_DIR` задаёт внешний каталог screenshots; в Git они не добавляются.
jsdom regression дополнительно проверяет зеркала каталога, один intent/счётчик, фильтры, Undo,
RU/EN, independent Settings и disabled gates.

## П.2 — реальный execution path, ещё не общая транзакция

- Unins вызывает `GlassProductPlan::Build` **до остановки хостов**, после реальных MSI context/
  cached-file/root probes. Строки исполнения больше не выставляются вручную после остановки.
  Состояние/политика SUT повторно проверяются на worker boundary, в том числе для silent
  удаления Settings; module-risk consent не отменяет этот запрет.
- Setup product phase теперь вызывает реальные MSI/registry шаги внутри общего RunPhase,
  а не использует scheduler лишь для наполнения vector. Result различает attempted/completed
  masks; неуспешный шаг включён в attempted, потому что он мог уже изменить MSI. Отмена/ошибка
  прерывает фазу, сохранённые rollback boundaries не расширены. Маски пишутся в журнал отказа.
- Это не завершает п.2: Setup всё ещё создаёт install-only worker rows; общий discovery→Build
  для Setup, mixed Remove, единый transactional lifecycle и MSI rollback Unins остаются в работе.
  Не выдавать in-memory masks за persistent recovery journal.

## П.3 — привязка Current к identity и двум SHA-256

- Для вновь acquired/verified source публикация сохраняет защищённую sidecar `.sha256`:
  128 ASCII hex bytes — отдельные SHA-256 **исходного distribution MSI** и зарегистрированного
  Windows Installer LocalPackage. Их нельзя сравнивать друг с другом: Windows может удалить
  embedded cabinets в своей кэшированной копии.
- Автоматическое доверие Current теперь требует version/PackageCode, единственного machine
  registration, совпадения source digest и installed-cache digest. Проверяется и private staged
  copy. Отсутствующий/malformed/linked/reparse record либо изменённый MSI не считается baseline.
- Новый record можно создать только для acquired source с флагом подтверждённой верификации
  либо источника с уже проверенным record. Legacy Current не получает доверие из одного ACL/
  ProductCode/PackageCode: остаётся независимая signature/manifest проверка acquisition.
  Автоматический RegisterRepairSource тоже требует эту привязку; непроверенный legacy
  source не становится repair source лишь из-за совпадения PackageCode.
  Изменённые upstream DLL и original SUT MSI не переподписывались/не модифицировались.
- **П.3 не закрыт целиком:** миграция Previous/Retained в единственный постоянный Current,
  безопасное сохранение/перерегистрация всех kept sources, EXE updater/неограниченные user bak
  и persistent recovery ещё требуют завершения. Старая retention логика не объявляется удалённой. Старый Retained без record может дать явный incomplete-cache
  finish warning: это fail-closed, а не подтверждённая миграция таких repair sources.

## Проверки и ограничения

Production DOM и Chromium layout/skin/IPC tests PASS. Текущие Engine и PackageAcquisition
компилируются в GNU Windows x86 objects; это не MSVC/WiX/link/runtime.
Полный запуск 55 Python suites дал 43 PASS / 12 FAIL. Все 12 отказов отдельно воспроизведены
на чистом **d6e2187** в временном git-archive test tree: устаревшие source expectations и fake
Win32 stubs/definitions. После актуализации относящегося к этому этапу package-catalog теста
(он ошибочно требовал GLASS_EMBED_MSI) он также PASS, включая ASan/UBSan; остаются 11 baseline
отказов: module_scan, platform_mutex, preserve_registry, registered_paths, scan_protocol,
shell_install_root, task_close_cleanup, task_scheduler_stop, unins_close_state, unins_task_stop,
watch_lifecycle. Не заявлять, что весь репозиторный suite зелёный. Новые cache digest tests,
16384 policy +4096 scheduler, fixed-path order, installer order и остальные ранее зелёные suites PASS.

Платформа/службы не запускались/не останавливались. Новых EXE/DLL в проекте нет.

---

# Production product workspace: заметная переработка страницы — 2026-11-06

Пользователь подтвердил сборки **22b7822**, но не увидел нового UI. Это подтверждение
не распространяется на текущую ревизию; предыдущая сетка не была полноценным redesign.

- Переработан именно `res/slideshow.html`, который встраивается ресурсом IDR_HTML в Setup
  и Unins, а не демонстрационный/временный HTML. Вместо двухколоночной сетки — одноколоночные
  продуктовые строки слева и живой план справа. Сохранены окно 1000×600, HTML 540 px,
  внутренний список 330 px, scroll/edge masks и предупреждения с popup без нижних кнопок.
- В каждой строке архитектура, установленная версия, версия проверенного target (если известна),
  выделение выбора и кнопка действия. Кнопки вызывают существующий checkbox IPC отдельно для
  Settings x64 и x86; это не декоративные controls. В Setup предлагается текущая операция
  Install/Update/Repair либо Skip/Keep, в Unins — Remove/Keep. Счётчики изменений/сохраняемых
  продуктов и пустой план пересчитываются сразу. Заголовки fresh/maintenance/uninstall и RU/EN
  переключаются независимо от изменения метаданных продуктов.
- Native startup log помечает `Product workspace UI ... (revision 3)` для идентификации EXE.
  WebView получает ресурс в памяти с `Cache-Control: no-store`; устаревший browser-cache
  причиной прежней жалобы не установлен. Native fallback не объявляется переработанным workspace.
- Общий `GlassRegisteredMsi.h` теперь используется Setup и Unins непосредственно на границе
  MSI операции: registration context/presence, проверка SHA-256 исходного снимка и заново
  скопированного зарегистрированного LocalPackage, затем file roots. Замена MSI между preflight
  и выполнением блокирует шаг даже при прежнем ProductCode. Hash читает pinned single-link
  regular file, проверяет отмену между блоками; временная копия удаляется при отказе/успехе.
  Имя MSI в workspace формируется только из проверенной GUID-формы ProductCode.
- Исправлена Unins SUT ветка на 32-битной Windows: она не требует отсутствующий x64 probe.

**Пункт 2 остаётся незавершённым:** общий preflight guard — следующий подключённый кусок,
не общий transactional Windows executor. Discovery→Build→worker, mixed Install/Remove в Setup,
общий MSI rollback для Unins, подключение removal session и SUT/shell optional-helper preflight
ещё требуют реализации. Неработающие Remove actions в Setup не включены. Остальной согласованный
lightweight план также не объявляется готовым.

Проверки текущей ревизии:

- `maintenance_ui_dom_test.cjs` исполняет настоящий production HTML в jsdom@26: шесть строк,
  одноколоночное CSS-правило/список 330 px, отдельные Settings IPC, версии/выбор/план, warnings/Escape,
  переключение RU/EN, заголовки Setup/Unins, disabled gates. Запуск:
  `NODE_PATH=/home/user/.cache/glass-dom/node_modules node GlassSetup/tests/maintenance_ui_dom_test.cjs`.
  jsdom установлен только во внешнем cache; новой runtime dependency проекта нет.
- `registered_msi_mock.py` исполняет реальный shared Recheck с fake Hash/MSI APIs:
  digest mismatch, copy/hash/path/context/presence errors, cancellation, отсутствие продукта,
  unsafe identity и cleanup. Это не проверка криптографии Windows API на реальных файлах.
- Все 21 regression suites PASS, включая 16384 policy +4096 mixed scheduler случаев,
  fixed-path/rollback/cleanup/profile/theme/license/trust/module gate/build graph. Fixed-path mock
  актуализирован под общий guard и существующую семантику optional component ABSENT; UNKNOWN
  по-прежнему отклоняется, следующий установленный компонент обязательно проверяется.
- Текущий InstallEngine компилируется в GNU Windows x86 object. Полный Unins GNU object не собран:
  MinGW WRL не содержит Callback для существующего WebView кода; production shims не добавлялись.
  **MSVC/WiX/link и реальный Windows/WebView layout/installer runtime здесь не проверены**;
  jsdom не измеряет реальную раскладку, браузера для screenshot в окружении нет.

Платформа/службы не запускались/не останавливались. Новых EXE/DLL нет.

---

# История 22b7822: независимые Settings x64/x86, ещё без заметного redesign — 2026-11-06

По запросу «на следующем коммите первый результат с новым интерфейсом»:

- Production HTML и native fallback показывают две независимые Settings cards, с отдельными
  inspection reports/default selection/IPC/checkbox controls. На x86 Windows карточка x64 скрыта.
  Первая установка и Modify имеют разные заголовки. Сохранены layout 1000×600, список 330 px,
  внутренний scroll/edge masks, отдельный план и существующие warning popups.
- На чистой x64 установке выбираются обе Settings; в maintenance — независимо по состоянию
  каждой архитектуры. Выбор проходит через acquisition и две реальные MSI операции early-helper phase.
  Ярлык при установке только x86 Settings не указывает на отсутствующий x64 EXE.
- Planner переведён на шесть независимых продуктов; поддерживает explicit per-row mixed intents.
  Раздельные предупреждения о Platform без Settings соответствующей архитектуры. Unins preview
  также использует реальные Settings64/32 presence/selection, не агрегированный флаг.
- `GlassMaintenanceExecutor.h` задаёт общий порядок/фазы продуктовых шагов и stop-on-error;
  Setup main MSI sequence и Unins removal dispatch используют его. Это header существующих EXE,
  не новый DLL/EXE. Он не расширяет набор выбранных продуктов.
- Ранний rollback восстанавливает обе Settings директории, если были затронуты обе архитектуры,
  либо только фактически начатую архитектуру; Platform-файлы на module hold не перезаписываются.
- Исправлен legacy clean-reinstall purge: невыбранные продукты и Settings/SUT не удаляются каскадом;
  install-path metadata очищается только для действительно выбранных Platform/OpenGlass.

**Пункт 2 ещё не закрыт полностью:** общий scheduler/portable mixed planner подключены, но Windows
transaction orchestration/preflight/rollback остаются специализированными для Setup и Unins.
Смешанные install+remove в самом Setup UI пока не предлагаются: нельзя выдавать tested portable
mixed intents за готовый Windows runtime. Единый lifecycle и SUT/shell-specific preflight ещё нужно
довести перед включением removal actions в новом UI. Остальные пункты lightweight плана также не объявлены готовыми.

Проверки этой ревизии: 16 384 policy cases + 4 096 mixed executor cases; actual early MSI/purge,
SettingsBoth rollback, Settings-removal consent, module gate, Native trust/loader, saved-theme/profile,
cleanup/license/source/build-graph regression suites PASS. Production JS node --check, project XML,
UTF-8/resource checks, git diff --check PASS. GNU Windows x86 object compile InstallEngine,
ProductInspect и InstallRollback PASS; **это не MSVC/WiX/link/Windows runtime**.
Пользователь подтвердил успешные сборки предыдущей ревизии 0e4449a, не этой новой UI ревизии.
Платформа и службы не запускались/не останавливались.

---

# Удаление embedded MSI/Native: реализация, общий executor ещё не завершён — 2026-11-06

Это промежуточное изменение по поручению «1 и 2, затем проверка сборки», **не готовность обоих пунктов**.

- Setup и Unins больше не содержат ресурсов MSI/Native. Embedded Unins EXE сохранён отдельно.
  Удалены конфигурация/extraction/cleanup embedded Native, обязательный ресурсный preflight,
  публикация x86 Native в maintenance cache и обратная project dependency Setup → Native.
  Пять подписанных MSI публикуются существующим targets рядом с Setup; acquisition не ослаблена.
- Inspector использует acquisition для проверенных внешних target MSI и отдельные Settings64/32 reports.
  Integrity baseline — защищённый Windows Installer cache с совпадающими ProductCode/PackageCode;
  нет подстановки нового MSI по совпадению версии. Неизвестная integrity отображается отдельной причиной.
  Версия OpenGlass.dll для отображения сохранена, package update сравнивается по MSI ProductVersion.
- Исполняемые установленные Native проверяются WinVerifyTrust, SHA256 publisher pin к подписанному
  текущему EXE, PE/экспортам/protocol; отклоняются writable/reparse пути и заменяемые предки.
  Допускается совпадающий pinned self-signed publisher offline, но не bad digest/прочие trust errors.
  x86 loader сохраняет loaded-path/export-alias/capability/protocol handshake. Native не извлекается в TEMP.
- Module hold в Setup/Unins: Retry / явный Continue at risk / Cancel. Неизвестный scan не считается
  пустым; Retry не вызывает force-unload/kill. Silent Unins требует отдельный
  `--accept-module-monitor-risk` (не отменяет запрет silent Settings+SUT или проверки MSI).

**Осталось по текущему поручению:** общий смешанный planner/executor и независимые Settings через весь
Setup engine ещё не реализованы. Unins removal executor и install-only engine пока раздельные.
SUT/shell-specific preflight может по-прежнему блокировать раньше module-risk страницы; это нельзя
выдавать за законченную интеграцию optional helper со всеми операциями. Полная миграция Current — отдельная задача.

Проверки: 16 regression/source/actual-method mock suites PASS, включая обновлённые Native loader,
signature-status и module-gate тесты; production JS node --check, XML/resource checks, git diff --check PASS.
GNU Windows x86 object compile: InstallEngine, ProductInspect, InstallUtil, InstallWatch PASS.
Это **не MSVC/WiX/link/Windows runtime**, Unins целиком в этом окружении не собран.
Платформа и службы не запускались/не останавливались. Новых бинарных модулей нет.

---

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


## Files-only SecureUxTheme: разделение presence и ownership (2026-11-07)

- Найдено воспроизводимое противоречие в исходниках: `Query` сохранял `INSTALLSTATE_ABSENT` как ownership candidate, а `IsInstalled` и inspector принимали любой непустой результат за installed presence. Причина конкретного Windows-наблюдения пользователя ещё не подтверждена; нельзя исключать настоящую DEFAULT-регистрацию или старый EXE без MSI evidence.
- `QueryInstalled` и startup `IsInstalled` теперь учитывают только DEFAULT. Консервативный `Query` по умолчанию **по-прежнему сохраняет ABSENT** для проверок контекста/владельца. DEFAULT не скрывается; DLL в System32 и Inactive сами по себе не являются установленным MSI.
- Provider Prepare и финальная граница InstallOriginalSecureUxTheme используют общий `QueryInstallPresence`: все family candidates проходят read-only context safety; ABSENT без контекстов не даёт ложный alreadyInstalled/no-op; чужой/непроверенный контекст блокирует операцию. Регистрации/файлы не удаляются и не «исправляются» для косметического соответствия UI.
- Inspector выводит MSI evidence в InstallLog и `presenceDetail` JSON, доступный через информацию о продукте (клик по имени): результат family query, итог presence, известные repack ProductCodes и состояния, family candidates и состояния. Числа состояния Windows Installer: DEFAULT=5, ABSENT=2, UNKNOWN=-1, ADVERTISED=1. Диагностический повторный query — отдельное read-only наблюдение, не atomic MSI snapshot.
- Available сохраняет визуальные состояния C. Добавлены независимые aria-label/data-catalog-state: доступен/не выбран, действие в плане, уже установлен/без изменений, запланировано удаление. Idle у kept Installed и planned больше не означает одинаковую семантику для доступности.
- Новая regression выполняет actual shared header + actual inspector presence projection: none/ABSENT/DEFAULT/repack/mixed/query error. Native provider test выполняет actual QueryInstallPresence: DEFAULT kept, ABSENT без контекстов отсутствует, foreign ABSENT rejected, enumeration failure/cancel/неожиданное состояние rejected. API context evidence здесь подменено, реальная Windows enumeration не проверена.
- Production DOM и Chromium покрывают исходное состояние **все отсутствуют, все unchecked**, SUT Available/active, пустой план; реальный drag Available→Installed даёт install IPC `:1`, обратный drag отменяет `:0`, без Remove/unsupported dialog. Отдельные registered removal cases сохранены. UI discovery/IPC response — fixture, не запущенный Windows backend.
- После изменений: **54 PASS / 11 прежних FAIL / 65 Python scripts**, DOM PASS, Chromium PASS, resource smoke PASS, diff-check PASS. Имена прежних 11 failures не изменились. Windows/MSVC/WiX/WebView runtime не выполнены; общий mixed Remove, rollback/recovery и standalone original SUT Unins этим не завершены. Платформа не запускалась/не останавливалась.


## Фильтры «нужно» и vendor row execution plan (2026-11-07)

**Уточнение пользователя:** SecureUxTheme действительно был установлен на его ПК. Его расположение в Installed корректно; больше не трактуем этот случай как files-only ошибку и не скрываем действующую регистрацию. Правила DEFAULT/ABSENT и MSI diagnostics сохраняются.

### Production UI

- Найдена отдельная фактическая CSS-ошибка: author `display:flex` у карточек перекрывал UA `[hidden]`. JS менял hidden/count, но браузер продолжал рисовать карточки. Scoped `.c-card[hidden] {display:none!important}` теперь исключает их и из layout, и из keyboard navigation.
- Available/needed = активные исполнимые предложения: не выбранные install/update/repair, либо отмена поддержанного pending Remove. Disabled/locked/applySupported=false не создают активное install/update/repair предложение. Pending Install остаётся только в Installed/needed, его catalogue alias idle и не проходит Available/needed.
- Installed/needed = запланированные действия и доступные update/repair по фактам продукта. В direct Unins фильтр показывает выбранное удаление. Healthy/kept и warning-only не проходят. `badgeTone=repair` больше не является доказательством повреждения: используем явный native `broken`.
- Production DOM и Chromium проверяют отсутствие видимых карточек при нуле активных предложений, disabled/locked/unsupported, warning-only, update/repair, pending install, сохранение All и отсутствие IPC при переключении фильтров. Chromium проверяет реальную видимость, а не только hidden attribute. Старые drag/pins/panels/geometry/content/removal tests сохранены.

### Нативный execution plan

- SUT больше не отсутствует в discovery→Build adapter: его Keep/Install/Remove представлен строкой в общем плане. Install допускается только при подтверждённом отсутствии installed original/repack и проверенной identity подготовленного MSI. Изменение регистрации не превращает выбранный Install в молчаливый Keep или Repair.
- Поддержанный оригинальный Remove повторно проверяет singleton inspected ProductCode, точный captured consent code, готовый baseline, strictly Inactive и RecheckRemoval **до остановки хостов/изменений Glass**. Противоречивые intents блокируются.
- Финальные vendor entry points требуют ready/valid план и точное действие своей строки. Install дополнительно повторно проверяет ProductCode/ProductVersion источника перед MSI; появившаяся после Build регистрация приводит к безопасному отказу, не к неявному upgrade/repair.
- Vendor row исключается из копии плана для обычной Glass Products-фазы: оригинальный MSI по-прежнему выполняется отдельно последней fallible транзакцией. Общий scheduler/порядок managed удаления в Unins не менялись. Это НЕ реализация общего mixed Remove/общего rollback vendor MSI.
- CheckLegacyRepackAbsent теперь проверяет контексты обоих старых Glass ProductCodes, прежде чем трактовать отсутствие DEFAULT как отсутствие препятствия. Чужой ABSENT repack не разрешает установку поверх его ресурсов. Автоматическая миграция/удаление чужого MSI не добавлялись.
- Actual-source tests расширены: 19 Build cases vendor install/remove с отказами identity/presence/context/activity/consent/baseline; финальный Install с проверками plan authority, changed MSI/source identity, success/failure/reboot; final Remove с invalid/missing/wrong-action plan; actual legacy context guard. Windows APIs здесь fake, не реальная проверка ACL/contexts/installer transactions.

Проверки после изменений: **54 PASS / те же 11 FAIL / 65 Python scripts**, production DOM PASS, Chromium PASS, resources PASS, git diff-check PASS. Windows/MSVC/WiX не запускались, платформа не запускалась/не останавливалась. Общие mixed Install/Update/Repair/Remove, shared rollback/recovery, original SUT standalone Unins, durable Current-only publication, self-renewal handoff и Windows acceptance остаются незавершёнными.


## ARP после частичного удаления и SecureUxTheme (2026-11-07)

Пользователь подтвердил успешную Windows-сборку предыдущего checkpoint `abaeb7e`. Это подтверждение не распространяется на следующие изменения ARP; новая MSVC/WiX сборка здесь не выполнена.

- Добавлен общий `GlassInstallMsi::RefreshManagedArp`: после MSI операций повторно опрашивает современные и legacy **фиксированные Glass ProductCodes**. Отсутствующие не получают новых ключей; чужие/неоднозначные/ошибочные регистрации не патчатся. Оригинальный MSI автора SecureUxTheme не входит в allowlist и его ARP не переписывается.
- `PatchArpUninstallString` возвращает фактический успех/отказ. Перед записью проверяются DEFAULT и context safety; target HKLM/HKCU выбирается по MSI context, без прежнего fallback HKLM→HKCU. Перед записью повторно проверяются presence/context. Другой registry view пробуется только при отсутствующем ключе, не при access denied или частичной ошибке записи.
- Проверяются не только две uninstall-команды, но и ModifyPath/DisplayIcon/NoModify/NoRemove/NoRepair и снятие WindowsInstaller. При частичной ошибке записи результат false, не «успех»; операции реестра не объявляются атомарной транзакцией.
- Settings `NoRemove` теперь учитывает original family inventory, старые repacks и неизвестную инвентаризацию, а не только два старых Glass SUT ProductCodes. Conservative ABSENT также сохраняет ограничение прямого удаления; это не installed presence UI. После подтверждённого отсутствия MSI guard значение возвращается к 0. Одни DLL в System32 не участвуют в решении.
- Setup после cache publication обновляет все remaining managed ARP entries (включая legacy), показывая finish warning при неполном успехе. Успешный original vendor-only Remove отдельно обновляет оставшиеся Glass записи: ошибка ARP — non-fatal finish warning после vendor commit, не причина отката авторского MSI.
- Unins создаёт scope guard до первой MSI removal: при ранней/частичной ошибке best-effort refresh не подменяет основную ошибку удаления. На штатном пути refresh вызывается явно один раз; его ошибка возвращает понятный partial-maintenance error, а не полный успех. Cleanup профилей не выполняется после такого отказа. Guard noexcept, включая allocation failure при логировании.
- Новый `arp_refresh_mock.py` компилирует actual writer/refresh и actual Unins scope guard с fake Windows APIs: original/repack/unknown NoRemove, сброс stale policy, vendor exclusion, modern/legacy, foreign/context change, HKLM/HKCU isolation, registry views, access denied, write error, interactive Modify fallback, early failure/success/no duplicate refresh. Это не Windows registry/context security/runtime validation.

Проверки: **55 PASS / те же 11 FAIL / 66 Python scripts**; production DOM PASS, resources PASS, diff-check PASS. HTML не менялся. Платформа не запускалась/не останавливалась. Общий mixed Remove/rollback, original SUT standalone Unins, профильные папки, durable Current-only publication/recovery, self-renewal handoff и Windows acceptance остаются открытыми; этот checkpoint их не объявляет реализованными.


## Критический runtime checkpoint: убрать неявную демо-установку (2026-11-07)

Пользователь подтвердил сборку предыдущего ARP checkpoint, но сообщил, что установка по-прежнему демо. Найдена точная причина в production main.cpp: отсутствие GLASS_EMBED_MSI направляло StartInstallation в 30-секундную симуляцию, а реальный engine call оставался в старой #else. После перехода на внешние MSI этот define отсутствует и в Release. Предыдущие engine/mock checks не проверяли фактический GUI→engine entry; это существенный пропуск.

- Startup discovery больше не удаляет orphan ARP автоматически до согласия пользователя, в том числе в demo tour. Очистка старых ключей не маскируется под обнаружение.
- Из production main удалены все GLASS_EMBED_MSI ветвления, неявные simulated install/rollback, их поля/таймеры и false completion. Debug и Release без demo-tour используют реальный InstallEngine и внешний acquisition path; embedded MSI не возвращались.
- На входе StartInstallation повторно проверяются license consent, допустимая страница, inspection readiness, selection, отсутствие debug tour и preflight. Сохраняются root identity и OpenGlass startup guards. Poll timer должен быть успешно создан **до** запуска worker; timer/Start failure приводит к Error, не к зависшему прогрессу/демо-успеху.
- Tick/отмена/завершение используют только реальный engine outcome. Прошедшее время не является доказательством успешной установки или отката. Набор selection и captured original-SUT removal code передаются в engine без подмены.
- Только явный Debug --demo-tour остаётся presentation-only. Он не вызывает StartInstallation/engine; pending/active tour блокирует запуск. При окончании сбрасываются preview signer data/trust consent/finish warnings, а WebView получает новые normal completion labels. Native и HTML body/note больше не переключаются в demo из-за отсутствия embedded payload. Текст обычного завершения нейтрален к составу: выбранные операции выполнены, без ложного заявления о запуске Platform при SUT-only Remove или Settings-only Install.
- Новый setup_real_entry_mock.py компилирует и выполняет actual StartInstallation/Tick + actual PhaseLabels в четырёх вариантах Debug/Release × legacy define present/absent. Fake engine/Win32 API: успешный запуск, consent/inspection/page/selection/preflight/path/OpenGlass/tour/timer/start отказ, captured selection/consent/code, отсутствие успеха по таймеру, реальные engine success/failure и rollback failure. Это не Windows/UI/MSI integration test.
- Полный Python suite: **56 PASS / те же 11 FAIL / 67 scripts**. Production DOM PASS, resources PASS, diff-check PASS. Windows/MSVC/WiX сборка этого изменения не выполнена; продукты/платформа здесь не запускались и не останавливались.

**Это критическая точка для новой Windows-сборки и smoke в изолированной VM:** впервые нормальный GUI действительно доходит до изменяющего систему worker вместо скрытой симуляции. Обе конфигурации теперь реальны; Debug без --demo-tour больше НЕ безопасный dry-run. До runtime proof нельзя объявлять real install/rollback готовыми. Рекомендуемая первая проверка — внешний MSI acquisition/preflight, затем один продукт, cancel/ошибка до MSI и успешное завершение с реальной регистрацией; журнал должен содержать InstallEngine::Start и конкретные acquisition/MSI результаты, а не simulated installation. Остальные незавершённые пункты общего плана остаются открытыми.


## Исправление ошибочного изменения режима Debug (2026-11-07)

Пользователь уточнил обязательный исходный контракт: **Debug — симуляция, Release — реальная установка.** В checkpoint 8b15042 я ошибочно распространил устранение неявной симуляции на Debug. Требование проверять реальный worker относится к Release; предупреждение предыдущего раздела о реальном Debug отменено.

- Восстановлены 30-секундная Debug-симуляция, её отмена/подтверждение/возврат и пятисекундный визуальный rollback. Все ветки ограничены `_DEBUG`, а не старым `GLASS_EMBED_MSI`. Debug не вызывает InstallEngine::Start даже при наличии legacy define.
- Release сохраняет реальный внешний MSI path, проверки consent/inspection/roots, timer-before-worker и обработку Start failure. Отсутствие embedded payload больше не превращает Release в симуляцию.
- Обычный Debug получает demo completion body/note и без --demo-tour. Автоматический page tour остаётся отдельным явным Debug-режимом. Native/WebView completion используют единое config-based правило.
- Регрессия actual StartInstallation/Tick/TickSimulation компилируется Debug/Release × legacy define present/absent: Debug — ноль engine starts, simulated completion/cancel/rollback, timer failure; Release — настоящий adapter entry, отказы и completion только по engine outcome. Используются fake APIs/engine, Windows runtime не запускался.
- Targeted entry tests (4 варианта) PASS, resources PASS, diff-check PASS. Windows/MSVC сборка исправления не выполнена. Полный проект/оставшиеся maintenance задачи этим не объявляются завершёнными.


### Fresh Release: локальные MSI и self-signed издатель

- Ошибка `No verified package ... Glass-x64.msi` на чистом ПК относится к acquisition, а не к наличию установленных продуктов. Конкретный исходный отказ на пользовательской машине без журнала ещё не установлен.
- `SameMsiPublisher` теперь проверяет MSI через WinVerifyTrust/SIP и разрешает `CERT_E_UNTRUSTEDROOT` **только** при точном совпадении сертификата с подписантом запущенного Setup; другие ошибки остаются фатальными. Сертификаты не импортируются. Это устраняет зависимость источника локального MSI от предварительного системного доверия к self-signed сертификату автора, которое устанавливается только отдельно по согласию.
- Отсутствующий/неоткрываемый файл, небезопасные атрибуты, несовместимые метаданные и отказ подписи/хэша теперь различаются в ошибке и журнале. Authenticode status записывается в журнал; неподписанный MSI не принимается автоматически.
- Read-only `gh release view`: текущий latest `v2.0.0.0070` содержит только Setup и version.json, без MSI. Внешний комплект для локальной проверки должен содержать MSI рядом с EXE; публикация нового полного каталога и MSI в scope этой правки не выполнялась.
- PASS: новый actual-source `package_publisher_mock.py` (успех, unknown root + exact leaf, отказ чужому/отсутствующему издателю, bad digest, no signature, expired, wrong usage, incomplete chain, generic trust failure, cleanup); `package_acquisition_mock.py`, `cache_digest_mock.py`, четыре конфигурации `setup_real_entry_mock.py`, resources smoke. Это portable API-seam тесты, не проверка реальных криптографических подписей Windows.
- Windows/MSVC/WiX и fresh VM runtime ещё не проверены. Следующая критическая проверка: подписанный Release-комплект с MSI в соседнем каталоге, чистая VM без сертификата автора, затем tampered/foreign/unsigned MSI; при отказе нужен новый GlassSetup.log. Debug simulation не менялась.

### Установленная Native DLL: регистрация меню и штатные Windows ACL

- Пользователь подтвердил прохождение установки после получения 81e1920. В новом журнале Setup все пять MSI завершились кодом 0. Сертификат Setup уже доверен (`trusted=1`, `WinVerifyTrust=0`), поэтому certificate page корректно не показывалась и повторный импорт не запрашивался. Предыдущая правка не отключала сертификат.
- Регистрация меню прервалась до запуска helper: `GlassSettings.Native.dll with the setup export was not found`. MSI verbose logs подтверждают установку DLL в правильный `GlassSettings` каталог. Старое сообщение объединяло отсутствие DLL, неверные экспорты, ACL и подпись; конкретный низкоуровневый отказ из этого журнала не восстановить.
- Найдена ошибка в `TrustedInstalledNative`: штатные MSI-owned объекты проверялись политикой приватного ProgramData cache, отвергающей `TrustedInstaller` и inherit-only `CREATOR OWNER`. Теперь отдельная installed-Native проверка валидирует файл и все предки, разрешает SYSTEM/Administrators/Windows TrustedInstaller, игнорирует не применяющиеся к объекту inherit-only ACE. Непривилегированные права изменения/замены существующего файла, незащищённые владельцы, reparse, неправильный тип объекта остаются запрещены; подпись/совпадение издателя и PE/экспорты сохраняются. Политика приватного кэша и ACL на диске не меняются.
- Добавлена диагностика этапов PE/export/ACL/publisher, исправлено вводящее в заблуждение `nothing trusted`: отсутствие нового запроса доверия не означает отключения/удаления имеющегося сертификата.
- PASS: `native_installed_acl_mock.py` выполняет настоящий ACL validator, TrustedInstalledNative и NativeDllForArch с fake Win32/SIP (стандартные Windows servicing/CREATOR OWNER ACL; отказ writable file/ancestors, owner/null DACL/reparse/type, missing file/export, foreign/unsigned publisher; cleanup). `shell_install_root_mock.py` обновлён с удалённого embedded preflight на проверку настоящего installed-helper gate, теперь PASS. Также PASS shell reinstall/sparse state, resources, четыре Debug/Release entry variants, diff-check.
- Реальная Windows регистрация compact/classic menu после этой правки ещё не проверена; старый пользовательский журнал доказывает отказ поиска/проверки helper, но не конкретное ACL состояние ПК. Нужна пересборка Release и повторная проверка с новым журналом. Debug simulation не менялась.

### Maintenance entry: запрос пользователя, пункты 3–5

- Пользователь подтвердил сборку b580b67; повторная установка проверяется поверх существующих продуктов. Исправление installed-Native trust/регистрации меню им пока не проверено, runtime acceptance для него не заявляется.
- Available изначально закрыт; существующие edge reveal/pin/hide и пользовательские изменения layout в сессии сохранены.
- Каскад карточек запускается при входе на страницу, с последовательными задержками 65 ms и движением/прозрачностью 360 ms. Пока report loading — ждёт его; обычное изменение выбора/host state не перезапускает каскад. Учитывается prefers-reduced-motion. Используется Web Animations API реального production HTML, без дублирующего макета.
- Полный `GlassProductInspect::Run` больше не стартует на Welcome/startup и не задерживает открытие maintenance. Стартует после установки Checking; переход к License/Components разрешается только после завершения проверки версии и применения отчёта/default selection. Обработаны оба порядка callback и offline catalog. Начальное read-only определение наличия продуктов/корней для preflight сохраняется; это не полный integrity scan. Исполнитель отдельно сохраняет обязательную повторную pre-mutation проверку MSI перед изменениями.
- PASS: новый `maintenance_entry_dom_test.cjs` (production DOM с API-seam animation/reduced-motion), новый actual-source `checking_inspection_gate_mock.py`, прежний production DOM test, четыре configuration entry tests, resources smoke, diff-check. Windows/MSVC/WebView rendered-frame runtime не проверен.
- **Пункты 1–2 не реализованы этим checkpoint:** общий Glass Remove в Setup всё ещё отсутствует. UI не получает removeSupported для managed Glass rows; selection/IPC не хранит общий removal intent, RunMsiSequence явно отвергает Remove. Нужна реальная mixed-action интеграция планирования, Settings-risk consent, сохранения helper до конца, MSI snapshot/rollback и reversible task/service cleanup, исключения удалённых продуктов из повторного запуска/регистрации и refresh ARP/cache. Кнопка без работающего adapter не включена; существующий original inactive SUT Remove не является общим решением. Это остаётся активной задачей, а не runtime-отказом завершённой функции.

### Общий managed Glass Remove: карточки → план → worker

- Добавлен независимый `Selection::removeMask` для пяти фиксированных Glass-строк (включая legacy family aliases); bool install choices по-прежнему означают только apply, false — Keep. UI/IPC принимает whitelist явных `:remove`, `:0`, `:1`, включает корзины, эхо operation=remove и отмену в Keep. Select-all не превращается в неявное удаление. Preview использует те же явные actions.
- `BuildExecutionPlan` независимо проверяет removal mask/противоречивые действия, OS support, установленную семью/context и snapshotted baseline. Remove не требует новой distribution MSI или release hash; чистое managed removal использует только защищённый installed snapshot, без release IO. Нативная Platform требует установленный совместимый Settings helper либо выбранную установку/repair helper в том же плане; отсутствие обнаруживается до Stop.
- Включён реальный `UninstallProduct` adapter для Platform64/32, Settings64/32, OpenGlass, текущих и legacy кодов. Перед MSI — повторный context/family/baseline check и NoteModified; после — проверка отсутствия активной регистрации. MSI failure/cancel идёт в Glass rollback, reboot-required — finish note, автоперезагрузка подавляется существующим MSI adapter. Разделение Debug simulation / Release actual не менялось.
- `GlassInstallRemoval::Prepare/Commit/Restore` теперь подключены к worker. Добавлен x64 task snapshot с корректным rundll architecture и root-only проверкой Platform tasks, без удаления чужих одноимённых задач в других папках. Снимки XML/security/enabled и службы/ярлыка до stop; shortcut удаляется только если не остаётся обеих Settings. Служба удаляется последней, после rollbackable регистрации; до этой точки failure восстанавливает MSI/files/tasks/shortcut.
- Removed rows исключены из успешного RestorePriorHosts и новой Current source publication; RegisterSelectedProducts использует только apply bool, поэтому task/service удалённых строк не создаются заново. Чистое removal не seed-ит AttachList/BlockList. Remaining managed ARP refresh сохраняется.
- Меню нативной Platform снимается через trusted installed helper до MSI removal, Settings удаляются после остальных removal rows. Новый opt-in Native export `GlassSettings_ShellExtUnregisterReportSetupV1W` сообщает no-op при отсутствующей регистрации и позволяет не создавать меню на rollback там, где его не было. Старый export сохраняет success=0 для совместимости старых Setup/Unins; новый Setup проверяет наличие reporting export и сохраняет legacy fallback. На старом Native точный no-op/rollback-state не доказан, поэтому для этого протокола требуется обновлённый helper. После успешного удаления меню/сертификат не регистрируются повторно. При rollback после изменения регистрации выполняется повторная регистрация owned root; failure учитывается в rollback result. **Ограничение:** compact/classic вид после rollback может измениться; exact shell-package/registry snapshot не реализован, подтверждена только логика попытки функционального восстановления, не Windows runtime/bitwise parity.
- Setup получил страницу Settings-removal-risk (HTML и native fallback), отдельное подтверждение пары архитектур, Back по умолчанию и повторную проверку актуального risk mask в worker. Изменение выбора сбрасывает consent, чужая архитектура его не заменяет. Другие продукты автоматически не добавляются в removal.
- **Явный scope guard:** managed Remove + original SUT mutation, а также force-clean purge + managed Remove, отказываются до изменений. UI предупреждает/блокирует одновременный vendor/managed Remove, backend независимо проверяет. Original SUT commit-last остаётся отдельным протоколом; общий атомарный rollback vendor/service/MSI этим не заявляется.
- Новые actual-source tests: `managed_removal_mock.py` (пять MSI adapters, legacy, snapshot before mutation, cancel/failure, unchanged registration/reboot, shell guard, helper preflight, shared shortcut); `settings_removal_consent_mock.py` (256 сочетаний, обе архитектуры, явное подтверждение, без каскада); production `managed_removal_dom_test.cjs` (5 корзин, IPC, host echo, plan, undo). Дополнены plan adapter, worker order и host restore tests. Устаревший private-cache ACL assertion в native_preflight приведён к installed-Native policy из b580b67.
- Полный запуск **72 Python scripts: 62 PASS / 10 прежних FAIL**: module_scan_mock.py, platform_mutex_mock.py, preserve_registry_mock.py, registered_paths_mock.py, scan_protocol_mock.py, task_close_cleanup_mock.py, task_scheduler_stop_mock.py, unins_close_state_mock.py, unins_task_stop_mock.py, watch_lifecycle_mock.py. Новый Remove/consent и целевые backend tests PASS; три production DOM scripts PASS. Это API-seam/source/portable tests, не Windows MSI, COM, SCM, package identity/ACL или rendered-frame runtime acceptance.
- Критический следующий этап: полная Release сборка x64, затем x86, включая новый Native; Windows VM: каждый одиночный Remove, полный Glass Remove, mixed apply/remove, Settings risk Back/accept, MSI cancel/failure с восстановлением, task/service cleanup и отсутствие повторного запуска. Native signing/ACL/menu fix предыдущего коммита по-прежнему не подтверждён пользовательским runtime. Сертификаты/профильная очистка/платформа в sandbox не запускались и не изменялись. Durable recovery/journal, exact shell rollback, vendor mixed atomicity и остальной C/native parity остаются открытыми.

### Исправление MSVC C2664 в managed Remove

- Пользовательская сборка выявила передачу `std::wstring` в `NextMsiVerboseLogPath(const wchar_t*)`. Вызов исправлен на `sink.label.c_str()`.
- API seam в managed_removal_mock.py приведён к настоящей сигнатуре (`const wchar_t*`, noexcept): прежняя подмена `const std::wstring&` скрывала compile error. Actual-source removal test и resource smoke PASS, diff-check PASS; повторная MSVC сборка и Windows runtime не выполнены здесь.
- Остаётся нереализованным: общая vendor/managed removal transaction, exact compact/classic shell rollback, durable journal/recovery, автоматический self-update handoff, завершение профильной очистки/уведомлений и полный C/native parity. Реализованный managed Remove, Native/menu fix и entry animation/Checking всё ещё требуют Windows acceptance.

### Checking handoff, ARP completion and OpenGlass residual inventory

- Maintenance native Checking no longer paints before a healthy WebView becomes ready: the existing loading surface remains, including reduced-motion/animation timer handling. Native Checking remains available on WebView failure. This addresses the native→HTML double-page path; rendered-frame behaviour still needs Windows verification.
- Interactive `Current/GlassUnins.exe` now waits for the private-temp child and returns its exit code, so the ARP caller does not receive success while the GUI is still running. Equality is against the validated maintenance-cache file. Product-local interactive `unins000.exe` still exits immediately to avoid holding its own MSI-owned image open. Existing silent waiting is unchanged. Actual Windows Settings list refresh is not claimed from an API-seam test.
- OpenGlass inventory probes service/tasks before the all-MSI-absent return. `installed` remains MSI-only; separate `residual` and `residualSafe` facts drive damaged Installed presentation and verified cleanup permission. Missing files alone do not establish a residual installation. Service/task paths and account/actions must validate against resolved protected/default roots; ambiguous/foreign paths cannot authorize deletion.
- Residual reinstall uses the normal verified-source Install path, not a fabricated installed MSI baseline. Explicit residual Remove authorizes only the existing auxiliary Prepare/Commit/Restore lifecycle, with configuration/task snapshots before Stop and service deletion last. Family/context/absence checks remain; an MSI appearing after inspection cannot be silently removed without its real snapshot.
- Dragging a selected installed repair/update card to Available now creates explicit Remove, rather than only unchecking the repair. A fresh pending install still returns to Available as deselection. Residual cards are not styled as fresh dashed installations; Remove/undo and reinstall are available through production IPC.
- Direct Unins post-MSI checks accept an auxiliary already disappearing during MSI removal, while refusing unknown queries, new unexpected backends and foreign paths. Added logs distinguish service/task query errors, changed presence, path rejection, and accepted DeleteService (whose final completion can wait for other open service handles). The exact cause of the user's previously retained disabled service is **not established** without its log; this checkpoint does not claim that every direct-uninstall failure has been reproduced or fixed.
- Portable checks: the full 73-script run after the implementation had **63 PASS / the same 10 historical FAIL**; the subsequently added `unins_handoff_mock.py` also PASS (74 scripts combined, 64 PASS / 10 historical FAIL). New actual-source `openglass_residual_mock.py`, extended discovery/removal adapter tests and three production DOM scripts PASS. These exercise fake read-only/mutation APIs, not actual SCM, Task Scheduler, file locking, Windows Settings or MSI.
- **Реализовано, но ещё не проверено на Windows:** all changes above; MSVC/WiX Release x64→x86 including Native; cold-WebView Checking, direct Settings uninstall/refresh, service/task cleanup, residual reinstall/remove and failure recovery. Prior Native/menu and mixed managed-removal runtime acceptance also remains open.
- **Остаётся нереализованным:** general vendor/managed atomic transaction and force-clean+Remove; durable journal/recovery and self-renewal handoff; exact compact/classic shell rollback; full native/C parity; standalone SUT migration; remaining profile cleanup/notices; shared Settings shortcut retargeting. Recovery of an arbitrary residual custom root after all protected root records were removed is not implemented: foreign/unknown-root cleanup fails closed, rather than trusting the service path as installation ownership.

### Explicit maintenance choices and planned-composition shelves

- Maintenance inspection no longer preselects repair/update or absent-product installation. Installed damaged/residual rows remain unselected/red, update offers unselected/yellow; fresh-install recommendations outside maintenance remain unchanged. Inventory conditions are not rewritten to obtain a colour.
- Setup Remove moves the authoritative card/input out of Installed into Available, active in both all/needed views. The removal intent remains in the execution plan and is not executed until Apply. Host echoes preserve placement; no stale Installed mirror remains. Undo from the plan restores Keep/Installed; choosing the available product (button or drag) replaces Remove with explicit Apply. Unsupported vendor reinstall remains blocked, with Keep still available. Standalone Unins layout is unchanged.
- PASS: production managed-removal, maintenance UI and maintenance-entry DOM tests; actual residual classification/default-choice checks; product policy/plan exhaustive tests; managed removal/discovery adapters; Checking gate; resources and diff-check. No MSVC/WiX or Windows runtime run for this correction.
- **Реализовано, но ещё не проверено на Windows:** default empty maintenance plan, red/yellow state presentation, Remove→Available and subsequent Apply/Undo in WebView, plus previously open ARP/service/native acceptance.
- **Остаётся нереализованным:** previously listed vendor/managed atomic transaction, force-clean+Remove, durable recovery/journal, self-renewal, exact shell rollback, full native/C parity, standalone SUT migration, profile cleanup/notices, Settings shortcut retargeting and unknown-root residual recovery. The previous user's direct-uninstall service failure still requires Windows log/runtime diagnosis.

### Task-scoped procedures, residual service 1060 and error-page layout

- User log `GlassSetup-5.log` proves OpenGlass MSI absent/files=0, residual service safe=1, selection only residual Remove; old worker copied 451153701 bytes, stopped both Platform architectures and started both Native scans. Final commit then got service-not-found 1060 and rolled back. The log does not establish who completed service deletion between probes; it does establish the false failure on an already satisfied Remove postcondition.
- Final service deletion now accepts 1060 even when the service existed at Prepare. No object is mutated in that case. Changed/new/running/unreadable service still refuses deletion; DeleteService remains last. Auxiliary failures no longer receive a fabricated MSI 1603 classification.
- Added `GlassProcedurePlan.h` and execution map in `procedure-map.md`: union of explicit row intents; snapshots restricted to selected MSI families and affected root/Settings leaf; affected-host capture/stop and rollback; separate x86/x64 scanner switches. OG-only does not scan Platform modules. x64-only scan does not eagerly load/scan x86 Native; an explicit Force Unload may lazily load its trusted controller through the existing checks. Shell registration/trust only follows selected native Platform Apply, not unrelated OG/Settings work.
- Pure residual OG Remove (current+legacy MSI both UNKNOWN, revalidated before mutations) uses only secured auxiliary workspace, service/task snapshots, relevant stop, validated cleanup and reversible auxiliary recovery. It does not snapshot Platform files/MSIs, classify platform presence, close Settings, start Native monitors, publish shared cache, restart kept hosts, restore unrelated files or wait the artificial five-second launch minimum. Mixed MSI/residual plans retain proper snapshots for their real MSI rows and do not fabricate an OG baseline.
- Error page uses a bounded flex column, independently scrollable/shrinkable detail area and reserved footer-note space instead of letting the note extend below the clipped mode panel. Visible geometry on the user's Windows DPI/WebView remains unverified.
- Portable regression: **77 Python scripts / 67 PASS / same 10 historical FAIL** before the final auxiliary classify-skip refinement; affected worker/resources tests re-run PASS afterward. New actual-source service-delete and CreateSnapshot tests, 486 Keep/Apply/Remove map cases, architecture-scoped unlock/loader tests and three production DOM tests PASS. No Windows/MSVC/WiX execution or rendered screenshot acceptance is claimed.
- **Реализовано, но ещё не проверено на Windows:** every procedure-map change and rollback combination, current residual-only repro, x86/x64 monitor selection, cache/ARP consequences, error footer at actual DPI, previous Native/menu acceptance. Full Release x64→x86 rebuild including Native and Windows VM failure/cancellation cases remain required.
- **Остаётся нереализованным:** architecture-specific closing of Settings processes (changing either Settings still closes all GlassSettings processes); fully per-file Platform root snapshots (a selected Platform root can include kept Settings files); general vendor/managed atomicity and force-clean+Remove, durable journal/recovery, exact shell rollback, self-renewal, full native/C parity, standalone SUT migration, profile cleanup/notices, shared Settings shortcut retarget and unknown-root residual recovery. Prior broad standalone-Unins scan policy is not refactored by this Setup checkpoint.

### Settings ARP architecture labels and remaining-work iteration

- Both Settings MSI project titles now explicitly contain `(64-bit)` / `(32-bit)`. ProductCode, UpgradeCode and package filenames are unchanged. Existing owned/context-verified Settings ARP keys receive the same DisplayName when the real ARP refresh runs during successful maintenance; Checking remains read-only. Rebuilding alone does not rename an already installed package until installation/maintenance updates it.
- Replaced basename-wide Settings close/query with selected-root process inspection. Only images matching the selected protected `root/GlassSettings/GlassSettings.exe` receive WM_CLOSE and participate in the 8-second wait. Another architecture or portable/same-name foreign path is preserved. Unknown process identity, enumeration error, cancellation and timeout fail closed. No process kill is introduced.
- Setup early-helper gate, pre-stop close and rollback host teardown use explicit Settings row bits. Shared host teardown no longer unconditionally closes all Settings. Standalone Unins passes its selected architecture bits; explicit all-profile cleanup passes both because that operation deliberately affects common configuration.
- Standalone Unins host capture/stop and Native scan now use separate x64/x86 flags. Settings removal stops its matching host, not both platforms; unrelated platform tasks/mutexes are not used as restart authority. Global profile cleanup still affects both by explicit user choice. This resolves the prior broad standalone-Unins stage-policy item, not original SUT standalone migration.
- PASS: full **78 Python scripts / 68 PASS / same 10 historical FAIL**; three production DOM scripts; extended ARP writer tests cover old-name refresh and DisplayName-write failure, project metadata matches writer. New actual-source Settings process test covers 32/64/both/none, same-name foreign copy, timeout, unreadable identity, cancellation and snapshot failure. Procedure-map test also exercises the actual standalone flag expressions (64 OS/selection combinations). No Windows/MSVC/WiX execution here.
- **Реализовано, но ещё не проверено на Windows:** package titles and old ARP-name refresh; scoped WM_CLOSE/query/wait in Setup, rollback and Unins; revised standalone stop/scan map. Previous procedure-map, service/ARP, Native/menu and error-page runtime acceptance remain open.
- **Остаётся нереализованным (пользовательский пункт «оставшиеся» ещё НЕ завершён целиком):** fully per-file Platform snapshot, general vendor/managed transaction and force-clean+Remove, durable recovery/journal, self-renewal handoff, exact shell rollback, complete native/C parity, standalone original SUT migration, completion of profile cleanup/notices, shared shortcut retarget and unknown-root residual recovery. These are not enabled by removing safety guards; destructive mixed operations continue to refuse until their lifecycle/recovery exists.
- ARP refresh now tries the known product architecture's registry view first (32-bit products use KEY_WOW64_32KEY), including on 32-bit Windows. The old missing-key-only alternate-view fallback and context recheck remain. Test verifies that an existing correct 32-bit key is updated without touching a stale same-code 64-bit key.

### SecureUxTheme offer versions and semantic hover

- Красные/жёлтые/зелёные HTML-карточки сохраняют доминирующий цвет при hover,
  включая warning/offer, damaged, activated и доступное обновление. Подтверждено
  production DOM и реальным Chromium, не только статическим чтением CSS.
- SUT Checking выполняет read-only Describe: hash/identity-verified локальная
  ProductVersion приоритетна, GitHub release metadata показаны отдельно. Без
  локального MSI целевая версия явно подписана «релиз». Numeric tag не выдаётся
  за MSI ProductVersion. Describe/Prepare делят canonical native lookup; общий
  alias не добавлен. Prepare сохраняет immutable snapshot/recheck и installed skip.
- Нативный fallback дополнен подписью версии SUT: для установленного — регистрация,
  для отсутствующего — проверенный local MSI либо явно подписанный release tag.
  Это частичное продвижение native parity, не завершение всего native/C backlog.
- Пользовательские журналы подтверждают соседний `SecureUxTheme_x64.msi` 4.0.0,
  затем защищённый Work snapshot; успешный MSI exit 3010 требует reboot.
  Snapshot в Work не означает сетевое скачивание.
- Добавлен actual-source `secure_ux_offer_mock.py`; расширены release parser,
  production DOM и Chromium проверки. Removal mock теперь ограничивает извлечение
  QueryInstallPresence до Describe; удаление по-прежнему тестируется прежним телом.
- Проверки: **83 Python scripts / 73 PASS / прежние 10 FAIL** после исправления
  границы removal harness; три production DOM и Chromium PASS, diff --check PASS.
  Детали локального прогона: `.cache/sut-offer-final/summary.json` (не Git artifact).
  Никакой новой Windows/MSVC/WiX сборки, MSI integration или native/DPI приёмки нет.
- Открыты: per-file Platform snapshot, durable journal/reboot recovery, self-update
  handoff/locked Previous cleanup, общая vendor/managed atomicity и force-clean+Remove,
  exact shell rollback, полная native/C parity, standalone original SUT migration,
  оставшиеся profile notices/cleanup и unknown-root recovery. Settings shortcut
  retarget выполнен отдельным предыдущим этапом; это больше не missing feature.

### Панель плана и диалоги: новый визуальный запрос

- «Удалить все продукты» — красная нижняя строка с SVG-корзиной; скрыта при отсутствии
  доступных для планирования удалений. Backend capability gates не обходятся.
- «Принять все рекомендации» вместо выбора всех: только обновления/повреждения или
  явная рекомендация, с per-row IPC; здоровые продукты остаются ручным выбором.
- Строки удаления красные, установки/обновления/переустановки зелёные. Панель содержит
  лишь краткую сводку и «Подробнее», без полных текстов уведомлений. Это явно заменяет
  прежнее пользовательское ограничение «предупреждения только на карточках».
- Диалоги: нейтральная glass-поверхность, размытие фона, kicker, разделители, строки
  сведений о продукте, нижний Esc/«Понятно», keyboard focus trap и возврат фокуса.
- Production DOM, Chromium, source resources и diff-check проверяются portable;
  новой Windows/MSVC/WiX или DPI-приёмки нет.

### Product dialog footer overlap and MSI diagnostic noise

- Причина пересечения: глобальный `footer` слайда задавал absolute positioning
  также footer диалога. Стиль ограничен `footer.slideshow-only`; footer диалога
  находится в обычном flex flow. Header/footer не сжимаются, длинный body имеет
  собственный scroll и не пересекает нижний разделитель/кнопку.
- `presenceDetail` (MSI inventory, GUID, rc/state) больше не отображается в
  пользовательском описании продукта. Диагностика сохраняется в исходном журнале
  `SecureUxTheme presence`; правила detection/removal этим не изменены.
- PASS: production DOM, actual-source presence mock, resources, Chromium geometry
  для короткого описания и длинных product/warning dialogs RU/EN, diff-check.
  Это не новая Windows/MSVC/WiX/DPI приёмка.
- Уточнение backlog: удаление оригинального SUT из Setup сейчас разрешено только
  для подтверждённого Inactive. Полное удаление Active/ActivationPending/
  DeactivationPending с безопасным handling стиля/reboot ещё не реализовано;
  также требуется конкретная причина недоступности удаления вместо общей подсказки.

### SecureUxTheme removal availability: explicit refusal rather than a demo-like stub

- Единая read-only native policy возвращает и capability, и RU/EN причину отказа:
  checking not ready, missing/ambiguous original MSI, active, pending restart,
  foreign, conflicting/unverified state. Разрешён по-прежнему только точный
  установленный original MSI + Inactive; мутационные предохранители не ослаблены.
- JSON `removeBlockedReason` передаёт понятное объяснение без inventory GUID/rc.
  У установленного SUT корзинка остаётся видимой при блокировке, но открывает
  объяснение (также с клавиатуры), а не отправляет Remove. Причина доступна в
  продуктовой информации и drop feedback. Bulk Remove не включает заблокированный SUT.
- Durable fixtures `sut-active` / `sut-pending` — демонстрации, не результаты
  обследования Windows. Actual-source native tests исполняют policy/IPC для всех
  семи состояний RU/EN и отсутствия MSI/identity/readiness. DOM проверяет отказ
  без IPC и без изменения намерения, в том числе для disabled карточки; Chromium
  проверяет настоящий Enter/Escape/focus и отсутствие bulk удаления.
- PASS: **83 Python / 73 PASS / те же 10 historical FAIL**, без новых падений;
  три production DOM и Chromium; source resources и diff-check. Полный Python log
  находится в локальном `.cache/sut-removal-reasons-final`, не Git artifact.
- **Не закрыто этим этапом:** полное удаление активного/pending SUT, safe style
  reset/reboot/rollback и standalone original MSI migration; остальные journal,
  cache handoff, per-file snapshot, mixed atomicity, exact shell rollback,
  native parity, profile/unknown-root пункты остаются открыты. Windows/MSVC/WiX
  и реальные MSI/COM/reboot проверки этого этапа не выполнялись.

### SecureUxTheme: distinguish pending first load from a loaded hook

- Исправлено предыдущее слишком строгое Inactive-only правило: оригинальный MSI
  разрешено удалять в Inactive **или ActivationPending**, то есть до первой загрузки
  hook. Одинаковое условие действует в capability, Prepare, Build и непосредственно
  перед vendor MSI. Это не снятие identity/context/baseline/conflict guards.
- Проверены GlassSettings `ActivationRequiresRestart` / `LoadedEventPresent` и
  native state projection. Original v4.0.0 Package.wxs задаёт безусловный
  ScheduleReboot; один код 3010 не доказывает runtime-необходимость перезагрузки.
- Успешные install/remove уточняют restart notice по свежему helper state: fresh
  ActivationPending требует restart даже при MSI 0; удаление до загрузки не требует;
  установленный и уже Active не требует новой перезагрузки. Raw MSI result логируется.
  При unknown state сохраняется vendor request; 1641 никогда не подавляется.
- Перед подавлением advisory 3010 read-only проверяется Session Manager
  PendingFileRenameOperations. Наличие операций, ошибки доступа/типа/формата,
  изменение размера/слишком большой payload сохраняют запрос. Registry не меняется.
- Actual-source worker tests: install→remove-before-reboot, already-loaded reinstall,
  runtime restart even with MSI 0, 3010/1641, pending file queue/unreadable guard;
  новый actual-source registry guard test; native IPC и Build проверяют ActivationPending.
  Chromium fixture sut-pending теперь действительно планирует Remove; loaded
  refusal демонстрируется отдельно sut-active / sut-unloading.
- PASS: **84 Python / 74 PASS / прежние 10 FAIL**, новых нет; три DOM и Chromium,
  resources/diff-check. Logs: `.cache/sut-runtime-restart-final` (не Git artifact).
- **Остаётся:** полное удаление уже загруженного Active/DeactivationPending hook
  со style/session/reboot/rollback lifecycle и standalone migration. Модель restart
  не является доказательством Windows integration; MSVC/WiX и VM install/remove/
  reinstall/locked-file acceptance ещё не выполнялись. Остальной backlog не закрыт.

### Log-confirmed unloaded removal: scope reboot queue to SecureUxTheme files

- Пользовательский GlassSetup-8: native state 3 до удаления, 1 после; MSI 3010,
  restart=1. Hook не был загружен. MSI-журнал показывает MsiSystemRebootPending=1
  до удаления и ScheduleReboot после завершения. Содержимое общей очереди и ошибки
  её чтения старый Setup не логировал; её конкретные записи по этим логам неизвестны.
- Ошибка предыдущего guard: любая общая pending-file запись сохраняла reboot notice
  именно SecureUxTheme. Guard заменён на QuerySecureUxPendingFileOperations: read-only
  source/destination пары, системные SecureUxTheme4.dll / ThemeUiProxy.dll в System32
  и SysWOW64; NT DOS, extended DOS, SystemRoot и overwrite-marker. Однозначно чужие
  операции не учитываются; неизвестные SUT namespaces/short paths, некорректный формат,
  inaccessible/changed data по-прежнему сохраняют запрос консервативно.
- Queue не изменяется. В лог добавлены read/failure reason, число пар и наличие
  релевантных операций, сами потенциально относящиеся к SUT пары. MSI 1641, loaded
  state и общая Windows необходимость reboot не скрываются этой правкой.
- Actual-source parser tests: native/WOW, source и destination, deletion/overwrite,
  mixed queue, unrelated DLL и одноимённый файл в Temp, unknown namespace, malformed
  pairs/tail/termination, unreadable/oversized/racing registry и Windows directory failure.
- PASS: 84 Python / 74 PASS / прежние 10 FAIL, новых нет; resources и diff-check.
  Logs: `.cache/sut-scoped-reboot-final`. Windows/MSVC/WiX и повторение именно
  пользовательского сценария на Windows не выполнены. Cache controller/self-update,
  обновление unins и удаление loaded SUT этим этапом не реализуются.

### Windows evidence: fresh install, restart, loaded hook, blocked Setup removal

- Пользовательский тест: начальный reboot → установка локального original SUT MSI
  4.0.0 → restart notice → reboot → custom styles работают, GlassSettings показывает
  включённое состояние без reboot notice → вход в Setup maintenance.
- GlassSetup-9 подтверждает source=local и ProductVersion 4.0.0, успешный original
  MSI result 3010, native post-install state 3 (ActivationPending). MSI log подтверждает
  успешную установку и ScheduleReboot. GlassSetup[2] после reboot: native state 2
  (Active), original vendor MSI зарегистрирован; сеанс заканчивается без запуска
  InstallEngine/MSI удаления. Это штатно сработавший незавершённый Active guard,
  не отказ vendor MSI и не ошибка определения loaded state.
- Подтверждены пользователем нормальная установка, переход Pending→Active через
  reboot и работа custom styles. Удаление loaded SUT этим тестом не проверено:
  доступ к нему пока закрыт в коде. Повторять такой же тест до реализации смысла нет.
- Нужна реализация lifecycle, а не снятие одного UI guard: подготовка/проверка
  visual style и исходного пользовательского контекста, обработка других профилей/
  сеансов и частичного reset, повторная MSI-авторизация, vendor uninstall с reboot
  suppressed, проверка after-state/restart. Существующий style-preparation Native
  helper текущего пользователя не равен реализации всего этого lifecycle.
- Загруженные три исходных журнала удалены после анализа по пользовательскому
  правилу; выводы сохранены здесь без копий журналов. Изменений removal capability
  и разрешения Active-удаления в этом checkpoint нет.

### Loaded SecureUxTheme removal — first restricted implementation

Replaces the blanket loaded-runtime UI refusal with an intent that must pass
worker-side authorization. Scope is deliberately **exactly one ProfileList user
profile**, matching the elevated/current desktop user, with no other user sessions.
Unknown runtime/style, incomplete/changed profile inventory, external/legacy MSI,
missing CurrentTheme, or inaccessible selectors refuse before vendor removal.

The installed OS-native GlassSettings DLL must expose the trusted **style-only
`GlassSettings_SecureUxThemePrepareRemovalV2W`** contract. Older DLLs are not called
under an assumed new contract: update that GlassSettings MSI first. Removing that
OS-native Settings product in the same loaded-removal plan is refused before
mutations; retain it until the vendor transaction finishes. No new native helper
is downloaded for vendor-only removal.

Before the last original-MSI transaction, Setup rechecks profiles/sessions and the
CurrentTheme snapshot, runs the native stock-style helper, verifies both the live
style and persisted ThemeManager/DllName as exact, non-reparse, trusted Windows
stock resources, then writes/verifies only this user's Themes/CurrentTheme link to
Windows Resources/Themes/aero.theme. It does not apply an entire theme or directly
change wallpaper/sounds/colors, edit custom theme files or other profiles. A later
check revalidates stock style, selectors, desktop identity, profile inventory and
original MSI identity before invoking the vendor. Loaded removal always keeps a
restart requirement even when the MSI returns 0 or a later query appears inactive.
Unloaded ActivationPending cancellation retains its existing no-restart refinement.

Failures after style preparation explicitly warn that style may already be reset;
MSI failure does not automatically restore the unsafe custom style. This is **not
an atomic multi-product transaction**: earlier Glass steps may already have run.
Profile/WTS rechecks do not prevent a new concurrent logon after the last check;
there is no session broker, durable recovery journal or offline/multiuser style
lifecycle yet. Strict stock-resource trust checks can conservatively refuse an
otherwise usable style; never interpret such a refusal as vendor MSI failure.

Portable validation: 80 `*_mock.py` tests, 70 pass, the same 10 known failures
(module_scan, platform_mutex, preserve_registry, registered_paths, scan_protocol,
task_close_cleanup, task_scheduler_stop, unins_close_state, unins_task_stop,
watch_lifecycle); no new failures. New actual-source profile/style harness uses
ASan/UBSan and covers profile/session/SID/selector changes, stock verification,
no preflight writes, one-shot commit and partial write failures. Worker/IPC/plan
harnesses cover loaded authorization, missing helper, helper/selector refusal,
retained-Settings requirement, MSI failure and reboot. Three package/product/release
runners plus resource/build-graph checks pass. Targeted tests rerun after final
stock-selector/reboot checks. **No Windows/MSVC/WiX build or real loaded uninstall
was performed in this Linux environment.** The previous Windows install/reboot
result is not evidence for this new path.


### All-profile loaded SecureUxTheme removal (supersedes the restricted scope above)

Following the requested simplification, removed the single-profile, desktop-SID,
other-session and Authenticode gates from the loaded-removal profile preparation.
All inventoried loaded/offline user hives now receive stock `CurrentTheme` and
`ThemeManager/DllName` references, with per-value readback. Missing selectors can
be created; existing custom links need not be readable during preflight. The
native V2 helper still switches the current session visually. Other sessions are
not forcibly switched: reboot/next logon completes their transition. No wallpaper,
sound/color values or custom theme/style files are edited by the selector writer.

Inventory/write failures continue to stop the vendor MSI with profile/value
context; partial changes are retained and reported rather than restoring custom
styles. Rechecks cover all written selectors, current-session stock path and
profile inventory. Stock paths must exist without reparse traversal, but do not
require signature validation. No global logon/theme-setting lock or atomic
multi-profile recovery is claimed. The installed V2 helper and retaining its
OS-native Settings MSI through the final transaction remain requirements.

Validation: all 80 mock scripts rerun, 70 PASS, the same 10 previously documented
failures, zero new failures. Replaced the restricted style harness with actual-source
all-profile tests (1/3/7 profiles, both selector writes, missing values, each partial
write failure, inaccessible profile, create/readback errors, changed inventory and
selectors, stock aero/aerolite paths). ASan/UBSan enabled. Worker, plan, IPC,
profile-inventory, resources, build-graph and package/product/release runners pass.
No Windows/MSVC/WiX build or real all-profile removal was run here.

### SecureUxTheme card: no stock-style preparation warning

Removed the automatic stock-style/profile-reset notice from SecureUxTheme's
product-card warning payload. Active runtime is not a warning and must not make
an otherwise healthy installed card yellow or offer that warning popup. The SUT
card now emits an empty generic warning; runtime activity/pending-restart labels,
inspection condition, removal blocking diagnostics and actual worker stock-style
preparation/reboot handling are unchanged. Other products' plan warnings are not
modified. Added a source regression assertion to the native IPC harness.

Validation: IPC/card source assertions, actual removal worker, all-profile style
harness and resource invariants PASS; git diff --check PASS. No Windows build or
browser/Windows visual acceptance performed for this change.

### Windows removal attempt: installed helper V2 dependency failure

Analyzed uploaded GlassSetup-10.log (session 2026-11-09 10:10–10:11).
Native query returned 2 (Active), original MSI ProductCode
{D686CD0E-139E-4599-A4E2-1B9BC0B72F73} was installed (state 5), local x64 MSI
ProductVersion 4.0.0 was found. Removal stopped before MSI changes because the
installed OS-native GlassSettings.Native.dll did not export
GlassSettings_SecureUxThemePrepareRemovalV2W. No profile/style preparation or
vendor uninstall was attempted. This is a Setup-introduced helper version
requirement, not a failed Windows style switch or vendor MSI uninstall.

The V2 export currently forwards to SetupV1W; requiring it adds no new switching
implementation. This dependency must be reconsidered: publishing a new Setup
alone should not require replacing installed Settings just to remove the vendor
MSI. A standalone Setup-owned preparation implementation/helper is preferable
to blindly invoking an older installed deactivation export with potentially
different registration/file side effects. This entry records diagnosis only,
not removal of the dependency. Uploaded source log deleted after analysis.

### Settings-only setup: retained platforms are not stop/scan targets; Unlocker layout

Windows evidence from GlassSetup-11.log and both 20261109_102935 Settings MSI
logs: selection x64=0/x86=0, mutation mask 0x24 (Settings x64+x86 only).
Nevertheless both platform tasks were stopped: each graceful wait expired after
15 seconds; x86 reported 8 residual processes. Both Settings MSI replacements
completed with code 0. The subsequent module monitor timed out twice with 5
holders; at 10:39:02 it finally verified all modules unloaded. Thus the supplied
logs show an unnecessary platform lifecycle operation and persistent residuals,
not Settings MSI failure. They do not prove a Native unloading implementation
regression; user reports Settings/context-menu stop succeeds promptly.

Fixed the procedure-map bug: Platform64/Platform32 and ModuleScan now depend
only on their corresponding platform mutation bits (including explicit removal),
not Settings bits. Settings-only replacement still closes selected Settings
processes, but neither stops/restarts retained platforms nor scans their injected
modules. Selected platform x64/x86 operations remain architecture-scoped; existing
selected-platform startup logic is unchanged. Exhaustive 486-map test updated.

A concrete stop-path discrepancy was also fixed: Setup disabled wakeResiduals
while Settings enables it. Setup now requests the same cooperative window/thread
queue wake, and repeats it after validated Scheduler fallback with HostDead reason.
No remote FreeLibrary, process kill, Native hook/unload-path or path=1/2/3 change
was introduced. Added phase/wake diagnostics. This correction is not a Windows
confirmation that all unload failures are fixed.

Unlocker copy/list now occupy separate vertical lanes; copy has bounded scrolling
for unusually long localized/host messages. Holder report moved down to y=416,
row padding/gaps/module spacing reduced. Actual production HTML Chromium tests
verify RU/EN modules/process modes, no overlap, scrolling and compact row height.

Validation: 80 mock scripts, 70 PASS, same 10 documented failures, no new failures;
resource source invariants, targeted module-gate/procedure-map and Chromium layout
checks PASS. No Windows/MSVC build or stop/reinstall runtime validation here.
Analyzed uploaded log originals removed; screenshots retained.


### Windows retry and removal of the installed V2 style dependency

GlassSetup-12.log (2026-11-09 10:56–10:57): native runtime query succeeds with
Active=2, original vendor MSI installed state=5, local original x64 MSI 4.0.0
found. Removal again stops at the combined file/architecture/required-export
validation for installed GlassSettings.Native.dll and PrepareRemovalV2W. This
log does not independently distinguish an absent export from the other checks.
There is no style-switch attempt or vendor uninstall. GitHub discovery HTTP 403
is unrelated to this local preflight failure. Error 1625 was raised by Setup,
causing a misleading Windows-policy caption; no Windows policy denial is proven.
User reports having updated Settings; another Settings reinstall is not a fix
we should require. Uploaded original log deleted after analysis.

Implemented Setup-owned stock-style preparation using the same linked shared
ThemeHelper::SetSystemVisualStyle used by Settings (NormalColor/NormalSize,
EnableSystemFont flag parity), followed by exact live-style path verification.
No installed DLL, rundll32 or V2 export is used for this operation. Removed the
V2 capability gate. All-profile selector preparation, original vendor MSI
revalidation and loaded-runtime reboot handling remain. Preflight failures now
use ERROR_INVALID_STATE, not the Windows-policy/MSI-rejection code 1625.

Existing runtime queries still use the installed helper; the same-plan removal
of OS-native Settings remains blocked for that separate reason. This change
fixes the specific reported V2 barrier, not every possible profile/MSI failure.

Validation: 81 mock scripts, 71 PASS, the same 10 documented known failures,
zero new failures. New actual-source ASan/UBSan harness covers no installed
Settings path, missing stock file, invalid attributes, apply HRESULT/readback
failure, wrong live path and font-flag parity. Worker, resources and build graph
checks PASS. Shared helper include path verified. No Windows/MSVC/WiX build or
real loaded vendor uninstall was performed in this Linux environment.

### Recommendations label, OpenGlass payload version and deleted-local profile residues

1. The recommendations button now reads RU “Всё выбрано” / EN “All selected”
when the nonempty recommendation set is fully selected. Cancelling a recommended
plan action restores the original label and enables the button. No recommendations
still means disabled, not an arbitrary bulk reinstall.

2. OpenGlass display now has independent installed/target payload-version fields,
read from the OpenGlass.dll File-table entry in the verified installed MSI baseline
and selected local MSI offer respectively. Card/action display uses these fields;
MSI ProductVersion remains unchanged for identity, downgrade/update ordering and
execution. A release-only/unreadable payload version displays unknown rather than
misrepresenting the platform/bundle version as OpenGlass's version. If the UI
chooses a newer remote offer, a different local offer's payload version is not reused.
The old shared UI/uninstaller protocol retains its existing fallback when no new
payload-version field is supplied. No MSI version migration/downgrade is introduced.

3. GlassSetup-13.log (session 2026-11-09 11:44): Active=2, original MSI installed
state=5, local original MSI 4.0.0 found; refusal is now an offline hive open error
1009 for SID ending -1008, not the V2 helper gate. No style switch or vendor
uninstall occurred. User reports two deleted local profile directories whose
NTUSER.DAT files remain held open. The log alone does not prove file-lock ownership
or that this SID is one of those accounts; 1009 is not a sharing-violation code.

Loaded-removal inventory now excludes residues only when LSA proves the SID
belongs to this machine's local SAM and LookupAccountSid returns NONE_MAPPED.
This happens before accessing the directory/NTUSER.DAT and is rechecked against
ProfileList before commit. Domain/cloud/unresolved accounts are not silently
excluded. Confirmed deleted-account residues are logged. Other profile access
failures still report their SID; no system hive unload, profile deletion or
registry repair is attempted. General all-profile settings cleanup remains strict:
this exception is explicitly enabled for SecureUxTheme style preparation only.

Validation: 82 mock scripts, 72 PASS, same 10 known failures, zero new failures.
New actual-source ASan/UBSan local-account classifier tests cover local deletion,
existing/foreign/unreachable/unknown identities. Inventory harness tests ignored
unopenable deleted-local hive, account reappearance, and strict cleanup behavior.
Inspection/acquisition tests cover local offer metadata and independent payload/MSI
versions. Actual production DOM tests cover OpenGlass display and recommendations
label after accept/cancel. Managed-removal DOM, Chromium Unlocker, resources and
build graph pass. No Windows/MSVC/WiX build or real removal validation here.
Uploaded GlassSetup-13.log removed after analysis; screenshots retained.

### OpenGlass: actual installed DLL and independent upstream release discovery

Clarification: the previous change only displayed File-table payload metadata; it
still did not discover OpenGlass upstream releases. Implemented independent
read-only discovery at api.github.com/repos/ALTaleX531/OpenGlass/releases/latest.
Stable numeric tag versions are parsed separately from GlassPlatform's MSI catalog;
draft/prerelease/malformed/oversized responses are rejected. Discovery failure is
logged and never falls back to a bundle version labelled as an OpenGlass release.

Installed “local” version is now read directly from the registered OpenGlass root's
OpenGlass.dll fixed file-version resource, without loading/executing that DLL.
The local MSI offer's payload version remains read from its OpenGlass.dll File
entry. Available-card sources display installed local DLL, local package payload
(if known) and independent upstream “on server” release version. Newer upstream
version is compared numerically and its source label emphasized. A newer remote
GlassPlatform MSI no longer stands in for the upstream OpenGlass version.

Release tags are metadata, not verified downloaded DLL bytes; tooltip identifies
them as official release versions. Upstream release information does not authorize
silently installing OpenGlass's separate EXE or relabelling an older local MSI as
containing a newer upstream version. Existing MSI identity/update/downgrade gates
are unchanged. No upstream payload installer integration is added by this change.

Validation: 83 mock scripts, 73 PASS, same 10 known failures, no new failures.
Actual-source OpenGlass parser/PE-reader tests cover numeric tags, drafts,
prereleases, malformed responses and version-read failures. Production DOM test
checks independent local/package/server versions. Inspector-loop, resources and
diff checks PASS. No Windows/MSVC build or real Windows network/UI validation.

### Uniform Available-card version sources: server, then local

Removed the third “package” category from source rows. All six product cards now
use one renderer: “на сервере” / “on server” first, then “локально” / “local”.
SecureUxTheme no longer labels its server source “релиз”; removed the release
prefix from its card version line as well. Underlying release/MSI types remain
separate internally. OpenGlass keeps independent upstream release metadata.

When installed, local means the installed version (actual DLL version for
OpenGlass); otherwise it means a known local MSI offer (payload version for
OpenGlass). Do not invent local availability from targetVersion: generic product
JSON now includes the independently discovered localPackageVersion. Known server
versions are displayed regardless of whether that source is the selected target.
Unknown sources are omitted, and no local/server/package triplet is emitted.

Validation: production DOM covers all six products, RU/EN, installed and absent,
exact two-source order/labels, distinct installed/local-offer/server versions and
unknown-origin omission. Managed-removal DOM, resource invariants and diff check
PASS. No Windows build or visual Windows acceptance performed.

### Windows removal retry: offline NTUSER loader fallback (1009)

GlassSetup-14.log (session 2026-11-09 12:14–12:20): Setup 2.0.0.0098 queries
SecureUxTheme Active=2, original MSI registered state=5, local original 4.0.0 found.
OpenGlass upstream/local versions both 3.0.2.3749. Removal stops before style/MSI
operations on RegLoadAppKey error 1009, now for SID ending -1011 rather than the
previous -1008. This does not prove that -1011 is deleted, that -1008 was skipped,
or that a file lock caused 1009. No vendor uninstall has failed in this attempt.

Added a standard RegLoadKey fallback specifically for ERROR_BADDB (1009) from
RegLoadAppKey during SUT profile preparation. Ordinary user NTUSER hives rejected
by the application-hive loader can now be offered to the normal registry loader,
including its standard transaction-log handling. Backup/Restore privileges are
enabled only around the operations and their previous states are restored. A
fresh GUID-named HKU GlassSetup-Offline mount is owned only after successful load;
Profile RAII closes its handle and unloads only that owned mount, including moved
profiles, failure and partial-inventory exits. Existing HKU SID mounts are only
opened/closed, never unloaded. No file deletion/copy/hand-written registry repair.
The fallback is not enabled for general settings cleanup.

If the normal loader also fails, Setup reports both loader codes rather than
pretending the profile was prepared. Truly corrupt/unreadable hives are not
silently ignored. Added immediate per-profile account classification/skip logging
so a later failure cannot erase evidence about earlier deleted-account residues.
Added path, owned mount, load and cleanup diagnostics. Normal owned mounts are
scoped to Inventory lifetime, not process-private app hives: abrupt termination
can leave an owned HKU mount, and unload failures are logged (no automatic global
mount recovery/journal is implemented). A regular fallback load may perform
Windows hive-log recovery; it is not a read-only file probe.

Validation: 84 mock scripts, 74 PASS, same 10 documented known failures, zero new
failures. Actual-source fallback/token-privilege ASan/UBSan harness checks load,
open, missing privileges, prior privilege restoration, no unload after a failed
load and cleanup retry. Inventory harness checks 1009 fallback, moved ownership,
strict cleanup isolation, unavailable fallback and handle release. Resource/build
graph/diff checks PASS. No Windows/MSVC build or real NTUSER load/removal was run.
This implements the missing alternate loader, not proof that the user's specific
hive will be recoverable. Uploaded GlassSetup-14.log deleted after analysis.

### Explicit missing-HKU-key skip policy replaces offline preparation

GlassSetup-15.log (2026-11-09 12:29): Active=2 and original MSI present. SID -1008
is logged as deleted-local and ignored. SID -1011 (C:/Users/WsiAccount) returns
lookup 122, then RegLoadAppKey 1009. The new RegLoadKey fallback succeeds (0),
but opening its mounted key fails with access denied (5); owned mount cleanup
succeeds (0). No visual switch or vendor MSI uninstall occurs. This is now a
registry-access barrier in preparation, not vendor uninstall failure.

Implemented the user's simpler policy: for SUT loaded removal, use only existing
HKU/<SID> keys. If missing, skip before validating profile paths/files and record
folder/SID for finish warnings. No NTUSER file probing/loading, SID-account lookup,
Backup/Restore privilege changes or global hive mounts. Removed the earlier
fallback header and deleted-account classifier. Strict general settings cleanup
retains its existing offline AppKey path; it is not changed into best-effort cleanup.
Access denied on an existing user key is still distinguished from an absent key.

Finish warnings are emitted separately for each skipped entry after successful
original MSI removal, alongside the loaded-hook restart requirement. They say
which profile/SID was skipped, that HKU is absent, and that its theme settings were
not modified. Missing keys are not described as proof of deleted accounts. Current
session stock-style application and opened users' selector reset remain. Recheck
allows stable skipped entries, but refuses if a skipped registry key appears
before removal. No profile directory deletion or forced system-hive unload.

Validation: 82 mock scripts (two obsolete helper tests removed), 72 PASS, same 10
known failures, no new failures. Actual inventory tests cover one/all missing HKU
keys, no offline loads, missing NTUSER with existing HKU, .bak residue, key arrival,
access denied and unchanged strict cleanup. Style harness covers zero/multiple
opened users. Worker harness verifies original MSI execution plus one warning per
skipped SID/path and reboot warning, with no warnings during preflight. Production
DOM/resource/build-graph/diff checks PASS. No Windows/MSVC build or actual vendor
uninstall verification performed here. Uploaded source log deleted after analysis.


### Log16: native-architecture stock-style preparation

Log16 confirms missing-HKU profile skipping works (1008/1011/1012).
Removal stopped before vendor MSI: the x86 Setup-owned SetSystemVisualStyle
returned 0x8007041D (service request timeout), not an MSI policy rejection.
The user reproduced incomplete visual-style application from GlassSettings-x86
on x64 Windows (error notice and incorrect scrollbar/background color pairing).
A matching msstyles path alone is not proof of complete color application.

Setup now delegates preparation to OS-native rundll32 and the trusted installed
GlassSettings.Native.dll export GlassSettings_SecureUxThemePrepareRemovalV2W.
On x64, only AMD64 Native is accepted; on 32-bit Windows, I386. This existing
export prepares/verifies stock style without unregistering SUT. Publisher/ACL,
PE architecture/export, launch, timeout and exit result are checked; worker
profile/style/runtime/vendor checks remain before MSI. There is no in-process
x86 apply fallback. IMPORTANT: NativeDllForArch in the current tree resolves an
installed DLL, not an embedded DLL. Missing/old native-architecture Settings is
therefore a remaining prerequisite; a self-contained bundled helper is NOT
implemented by this change. No new exports or Settings update is required if
PrepareRemovalV2W is already present. General GlassSettings-x86 theme application
is not changed here.

The temporary ThemeManager2/STA experiment was withdrawn after the architecture
clarification. Error title/body/generic detail are operation-neutral in RU/EN
resources and HTML (native fallback uses the resources). Preparation failures
no longer receive ERROR_INSTALL_PACKAGE_REJECTED and its false policy message.
Targeted production-source mocks, resources/build graph and production DOM pass;
Windows/MSVC/WiX and actual theme color/removal validation remain outstanding.


### Action-specific Native prerequisite for SecureUxTheme removal

Implemented the explicit planning dependency requested after Log16:
- Native architecture follows the OS. A newer available Settings package
  (verified local MSI / downloadable catalogue offer), damaged installed Settings,
  or missing OS-native Settings while the other architecture is installed requires
  an explicit Settings Apply intent. Selection never cascades automatically.
- Click and real pointer drag share the same guard. Refused removal leaves the
  card installed and posts a yellow top notice. The plan is numbered in execution
  order (Settings helper phase first, vendor SUT removal last). Cancelling or
  removing the prerequisite cancels the dependent intent, both optimistically in
  HTML and authoritatively in native IPC/Apply guards.
- With no Settings products installed, explicit removal accepts manual risk:
  red top notice plus existing compact warning-count/Details summary (red severity)
  while the removal remains planned. No extra confirmation dialog is introduced.
  Both Settings MSI registrations must still be absent at execution; missing DLL,
  untrusted Native, other-bitness Settings or broken/advertised registration never
  authorize this exception. The normal original-MSI baseline/identity/cache checks
  remain mandatory. Native runtime/style/profile preparation is explicitly not
  promised in this manual mode. Finish warnings repeat manual preparation/restart.
- Notices wait for the next pointer click anywhere in the window, minimum 1000ms;
  no subsequent activity means no timer-only expiry. Triggering input is excluded.
  Native fallback uses its own inline notice/timer, no system dialogs.
- The existing early Settings MSI phase already precedes vendor commit. When a
  native update is explicitly planned, old-Native query/profile preflight no longer
  blocks that prerequisite: original MSI is verified before any mutation; fresh
  runtime/profile/style checks run via the newly installed Native before vendor
  MSI. Failed update/check prevents vendor removal and uses normal Glass rollback.
  Retained Platform is not stopped/scanned just to update Settings.
- Detection is conservatively at Settings package-version/health level, not a
  per-DLL digest/version dependency solver. The documented rule is reusable for
  future action-specific dependencies; only SUT removal is wired currently.

Validation: 82 mock scripts, 72 PASS / 10 unchanged known failures
(module_scan, platform_mutex, preserve_registry, registered_paths, scan_protocol,
task_close_cleanup, task_scheduler_stop, unins_close_state, unins_task_stop,
watch_lifecycle). All four production DOM suites PASS, including new RU/EN
click/real-drag, wrong architecture, prerequisite order/undo, manual risk/Details,
host echo and deterministic notice timing. Resource/build-graph/diff checks PASS.
No MSVC/WiX build, actual Windows theme color validation or live MSI acceptance
was possible here; mocks do not establish Windows runtime success.

Contract updated alongside code in cache-profile-maintenance-contract.md and
linked from the module README. Previous installed-Native requirement is now
conditional: explicit newer-Settings prerequisite or explicit absent-Settings
manual exception; no embedded executable payload or new Native export was added.


### Уточнение: только клики закрывают уведомления

По уточнению пользователя движения мыши, прокрутка, клавиатура и команды
контролов больше не считаются активностью для закрытия уведомлений. HTML
учитывает только pointer click/auxclick (не keyboard/programmatic detail=0),
с сохранением минимума 1000 мс. Завершающий click инициирующего drag исключён.
Клики в нативном footer/title bar передаются в HTML отдельным уведомлением.
Native fallback наблюдает завершённые нажатие/отпускание в окне и дочерних
контролах до IsDialogMessage; перетаскивание за системный порог не считается
кликом. Инициирующий клик не закрывает созданное им уведомление.

PASS: production dependency DOM (RU/EN: pointermove/down, wheel, keyboard и
programmatic click не закрывают; click/auxclick и footer закрывают; trailing
drag click исключён; минимум 1000 мс); actual native click observer mock
(client/child/nonclient/footer, foreign window/drag/keyboard/command exclusions).
Resources/diff checks PASS. Windows runtime validation remains outstanding.


### OpenGlass: версия payload отдельно от MSI-оболочки

Скриншот image-20 выявил рассогласование: отображается OpenGlass.dll 3.0.2.3749,
но жёлтый статус формировался по более новой версии Glass MSI. Теперь признак
обновления продукта OpenGlass определяется **только** численным сравнением
фактического установленного DLL и DLL в проверенном предлагаемом MSI. Равные
версии (включая 3-компонентную и эквивалентную 4-компонентную), более старая
или неизвестная версия предлагаемого DLL не дают статус обновления. Более
новая версия оболочки сама по себе не является обновлением OpenGlass.

Inspection, native JSON badge/state/action и HTML facts/recommendations согласованы.
Для remote MSI без проверенного payload не используется версия DLL из другого,
локального MSI. Upstream release metadata остаётся отдельной строкой «на сервере»;
она не разрешает установку upstream EXE и не подменяет доступный MSI payload.
Явная переустановка остаётся доступна; повреждение сохраняет статус восстановления.
MSI identity/version проверки acquisition и выполнения не изменены: новая оболочка
по-прежнему обслуживается штатным MSI, но не выдаётся за новую версию OpenGlass.

Регрессии: production DOM RU/EN — новая оболочка/тот же DLL, selected/unselected,
численное сравнение .9/.10, older/unknown/malformed DLL, настоящее повреждение;
C++ helper tests — равенство, нормализация, порядок, неизвестная/невалидная версия.
Windows/MSVC/WiX/runtime проверка здесь не выполнялась.

Итог проверок этой правки: 83 mock scripts — 73 PASS / 10 прежних известных
FAIL; все 4 production DOM suites, resources, SUT build graph и diff-check PASS.


### Уточнение: прокрутка тоже закрывает уведомления

По последнему уточнению пользователя к кликам добавлена прокрутка. HTML
учитывает wheel (мышь/тачпад, включая detail=0 и горизонтальную прокрутку);
Native fallback — WM_MOUSEWHEEL/WM_MOUSEHWHEEL/WM_VSCROLL/WM_HSCROLL.
Прокрутка вне WebView передаётся в HTML через component-notice-scroll.
Минимум 1000 мс сохранён; движение мыши и клавиатура остаются исключены.
Предыдущее правило «только клики» выше описывает историческое состояние.
Регрессии: реальный HTML RU/EN — wheel до минимума и native scroll после
минимума; actual native observer — вертикальная/горизонтальная прокрутка,
дочерние контролы, отсутствие дублирования WebView/чужих окон.

### Log17: исправлена неполная реализация stock-style Native export

Проанализированы GlassSetup-17.log и все четыре MSI-лога сессии 20261109_135954.
Оба Settings и обе Platform установлены успешно (MSI MainEngineThread return 0).
При удалении SUT в 14:01:57.512 реально запущен OS-native x64 DLL
C:\Program Files\GlassPlatform\GlassSettings\GlassSettings.Native.dll;
ran/started/completed=1, exit=0x0000000C. Отсутствующие HKU1008/1011/1012
пропущены как требуется. Vendor MSI removal ещё не запускался. После отказа
Glass products успешно восстановлены из rollback baseline. GitHub403 discovery
не является причиной отказа. Внутренний HRESULT по этим логам неизвестен.

Причина расхождения исходников: после исправления WOW64 routing V2 оставался
alias V1, чей ApplyStockVisualStyle вызывал только ordinal 65; рабочая Settings
кнопка сначала вызывает ThemeManager2.AddAndSelectTheme(.theme). Это упущение
предыдущего исправления, а не неустановленная зависимость/ошибка MSI.

Теперь Native StockVisualStyle helper воспроизводит primary-путь Settings на
собственном STA: тот же COM ABI/CLSID/IID/context, Init(0), .theme, flags32/4,
без IgnoreColor; ordinal65 только для EnableSystemFont после COM. Строгая
проверка HRESULT всех стадий и точного текущего stock-path. V2 вызывает helper
непосредственно и возвращает фактический HRESULT вместо generic 0xC; V1 сохраняет
bitmask ABI и использует helper в reset-path. No fallback к x86 Setup, ослабления
проверки по basename/ошибке HRESULT или попытки снять SUT registration нет.

Подробный контракт: GlassSettings/docs/stock-theme-maintenance.md.
Windows/MSVC/WiX acceptance остаётся обязательным; точную причину старого
внутреннего отказа без HRESULT не объявляем установленной. Загруженные исходные
логи после анализа удалены; image-21 сохранён.

Проверки Log17 fix: 85 mock scripts — 75 PASS / 10 прежних известных FAIL;
все четыре production DOM suites, resources, SUT build graph и diff-check PASS.
Новые actual-source тесты покрывают Native COM pipeline (ASan/UBSan), паритет
ABI/flags с C#, порядок ThemeManager→fonts→readback, cleanup/error paths и V2
HRESULT propagation/native-bitness/legacy/query guards. Windows runtime не проверен.
