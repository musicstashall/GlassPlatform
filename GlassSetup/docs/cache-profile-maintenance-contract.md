# Кэш, очистка профилей и сброс визуальных стилей

Дата: 2026-11-04. Уточнение после согласования редакции 2 таблицы состояний. Это проект реализации, не выполненная очистка Windows.

## Политика контроллера после Log24

Пользователь поручил выбрать процедуры по модели Visual Studio Installer без
дальнейшего согласования технических вариантов. Нормативная целевая цепочка и
границы относительно Microsoft guidance: [installer-servicing-policy.md](installer-servicing-policy.md).
Прежние пометки «не утверждено/нужно согласовать» в исторических Log24 разделах
описывают прошлое обсуждение, не новый запрос. Обновление controller независимо
от продуктовой транзакции; source coverage независимо от сохранения оболочки.
Это целевая политика, не утверждение о готовности runtime self-update.

## 1. Принятые решения пользователя

- Постоянно хранится только актуальный комплект; staging и rollback допустимы временно.
- При частичном обновлении источник старого продукта сохраняется только при фактическом отличии его версии; после обновления/удаления продукта ненужный источник должен удаляться автоматически.
- Перед глобальной очисткой проверяется доступность всех профилей. Обнаруженный отказ останавливает очистку до первых изменений. Ошибка после начала — частичный результат.
- Чужие сеансы не завершаются автоматически. Если безопасная работа невозможна, пользователь получает требование закрыть приложения/выйти из сеанса и повторить.
- При удалении SecureUxTheme требуется безопасно сбросить нестандартный визуальный стиль у всех затронутых пользователей, а не только у инициатора. Сброс стиля не зависит от checkbox «Удалить настройки».
- Headless remove GlassSettings при установленном SUT запрещён; через явный UI-план можно удалить их совместно или раздельно. Отсутствие Settings не запрещает SUT, предупреждение остаётся на ответственности пользователя.

## 2. Проверка DWM.txt

Экспорт пользователя прочитан из вложения, не включён в Git и не используется как исполняемый .reg. Найдено **228 значений**:

- 223 `DwmInitSessionActivityId_*`;
- `GradientWhitePixelGPUBlacklist`;
- `OneCoreNoBootDWM`;
- `ShaderLinkingGPUBlacklist`;
- в `ExtendedComposition`: `ExclusiveModeFramerateAveragingPeriodMs`, `ExclusiveModeFramerateThresholdPercent`.

Ни одно имя не совпадает с текущим `kDwmOpenGlassRules` в `GlassShared/src/RegistrySanity.cpp`.

Проверен исходный ALTaleX531/OpenGlass, commit `fb145fcd391b274cb91e642c93b3fc2b0e30e61c`:

- `Common/DwmSettings.hpp` читает пользовательский и machine DWM как уровни **настроек**, с fallback; HKLM DWM не следует считать исключительно кэшем OpenGlass.
- `Scripts/OpenGlass.iss::DeleteConfig` удаляет конкретный список значений HKCU/HKLM DWM, **не раздел целиком**.
- Поиск имён из экспорта (`DwmInitSessionActivityId`, GPU blacklist, OneCoreNoBootDWM, ExclusiveModeFramerate*) по исходникам не нашёл соответствующих writers/настроек OpenGlass. Это отрицательный результат для проверенного commit, не доказательство поведения всех сторонних сборок.
- Внешние публичные экспорты Windows/WinPE содержат такие же имена: https://gist.github.com/readloud/1d08c1d2a7e354fa9fd66b7ebd75e182 и https://coolvitto.hateblo.jp/entry/2025/01/31/182911 . Это вспомогательные, не официальные clean-image baseline источники.

Вывод: считать содержимое присланного HKLM/DWM мусором/кэшем OpenGlass **оснований нет**. Имена указывают на конфигурацию совместимости GPU, композиции и идентификаторы инициализации сеансов Windows. Точное значение/происхождение каждой записи и их наличие «по умолчанию» на конкретной сборке Windows не подтверждены чистым образом или трассировкой writers. Для deinstaller достаточно безопасного правила: все перечисленные значения и `ExtendedComposition` сохранить; удалять только подтверждённые собственные настройки OpenGlass по списку. Новые неизвестные имена сохраняются.

Источники кода:
- https://github.com/ALTaleX531/OpenGlass/blob/fb145fcd391b274cb91e642c93b3fc2b0e30e61c/Common/DwmSettings.hpp
- https://github.com/ALTaleX531/OpenGlass/blob/fb145fcd391b274cb91e642c93b3fc2b0e30e61c/Scripts/OpenGlass.iss

### Реализация registry cleanup после полного удаления (2026-11-04)

Повторно сверен `Scripts/OpenGlass.iss::DeleteConfig` на указанном выше commit:
upstream содержит 54 имени, локальный `kDwmOpenGlassRules` — 49. Разница ровно
пять штатных Windows значений: `ColorizationColor`, `ColorizationAfterglow`,
`ColorizationColorBalance`, `ColorizationAfterglowBalance`, `ColorizationBlurBalance`.
Это намеренное исключение по принятой политике, не недостающие настройки OpenGlass.
Добавлять их в destructive cleanup нельзя. Остальные 49 имён полностью совпадают.

`CleanOwnedSettings` вызывается общей Inventory из Setup и GlassUnins после
успешного полного удаления с явным согласием: каждый пользовательский hive и HKLM,
оба registry view. Удаление не зависит от типа значения. Добавлена отсутствовавшая
проверка результата: после прохода удаления перечитывается наличие всех 49 имён.
Если writer восстановил значение или query недоступен — cleanup возвращает ошибку,
а не объявляет реестр очищенным. Диагностика delete/verify содержит имя значения и
код ошибки. Это не атомарная блокировка реестра: будущую запись после проверки
исключить нельзя. Штатные/неизвестные значения и DWM key/subkeys сохраняются.

`GlassSetup/tests/registry_cleanup_mock.py` исполняет настоящую функцию на fake
registry: два user hive + HKLM, оба view, preflight без мутаций, удаление всех 49
имён, сохранение пяти Windows цветов и неизвестных имён, повторная очистка,
отказы доступа/delete/query и повторная запись writer. Это не Windows acceptance.

## 3. Как удалять старые источники без накопления версий

### Учёт, а не удаление по возрасту каталога

Manifest controller хранит:

- ActualInstalled: stable product ID, architecture, install context/SID, ProductCode, UpgradeCode, MSI version, bundle/display version, PackageCode и проверенный hash источника;
- CurrentSources: файлы актуального комплекта и покрываемые ими installed identities;
- RetainedSources: минимальные источники конкретных оставленных старых версий; не старый setup.exe/полный bundle целиком;
- TransactionPins / RecoveryPins / RebootPins: временные ссылки незавершённых операций;
- Journal: до/после, результат MSI и причина удержания каждого файла.

Для продукта той же версии источник повторно не сохраняется, если актуальный пакет действительно обслуживает тот же установленный продукт/context/архитектуру. Один текстовый номер версии сам по себе не доказывает эквивалентность двух перепакованных MSI: отличающийся PackageCode/payload — конфликт источника, требующий проверки, а не повод удалить последний работоспособный MSI. Не плодить копии одного hash. Различие разрядности не считать исторической версией.

### Порядок обновления старого продукта

1. Зафиксировать machine-wide operation lock. Перечитать фактически установленные MSI и текущий manifest; не доверять прошлому UI-снимку.
2. Защитить нужный старый источник transaction pin, проверить новый источник и staged controller.
3. Выполнить update, сохранить точный код MSI. При ошибке оставить старый source для rollback/recovery.
4. После успеха проверить фактическую регистрацию/версию и пригодность нового источника для обслуживания, атомарно записать новый manifest.
5. Снять installed reference на старый source. Если есть reboot/recovery/rollback pins — пока не удалять.
6. В commit/finalize пересчитать ссылки. Старый source с нулевым числом ссылок удалить; отметить результат GC в журнале. При ошибке удаления отметить `CleanupPending`, не возвращать ложный полный rollback установки.
7. Повторить GC при следующем запуске maintenance/после завершения reboot recovery. Успех повторного прохода не требует повторной установки продуктов.

Тот же GC выполняется после удаления продукта, завершения отмены/rollback, восстановления оборванной транзакции и смены актуального комплекта. Для отказавшего обновления старая версия остаётся фактической и её source сохраняется.

Удалять только принадлежащие controller файлы внутри защищённого Glass cache, после проверки identity/ACL/reparse. Не чистить `%WINDIR%\Installer`, общий `%ProgramData%\Packages`, чужие источники или файлы по правилу «всё старше N дней». Reparse/подозрительный manifest — отказ и диагностика, не эвристическая чистка.

## 4. Сброс визуального стиля всех пользователей перед удалением SUT

### Различаем результат и способ

Требование сбросить custom style принято. Предложение «после выхода пользователя отредактировать реестр» разумно как offline-ветка алгоритма, но **ещё не проверенный полный рецепт**.

В текущем коде `SecureUxThemeNative.cpp` проверяет `ThemeManager\\DllName`, затем применяет штатный `.msstyles` через `Shared::ThemeHelper::SetSystemVisualStyle` — только для текущего пользователя. `GlassSettings/Services/VisualStyleThemeManager.cs` дополнительно учитывает `Themes\\CurrentTheme` и содержимое `[VisualStyles]` файла `.theme`. Это разные уровни состояния; замена только DllName или только CurrentTheme не гарантирует, что при входе не восстановится custom style.

### План по состоянию профиля

| Профиль | Предлагаемая обработка |
|---|---|
| Текущий пользователь, активная сессия | Helper подходящей битности в исходном пользовательском контексте применяет штатный visual style; затем повторно проверяет фактическое состояние |
| Другой активный/отключённый, но не завершённый сеанс | Нельзя считать профиль offline. Не редактировать его hive конкурентно как substitute live apply. Предложить выход из сеанса/повтор; при наличии проверенного session-helper возможна отдельная live-ветка, но не обещать её сейчас |
| Пользователь вышел, hive доступен и нет иных сеансов SID | Проверенный offline-reset только visual-style selectors и связанных восстановительных ссылок; не чистить произвольную персонализацию |
| Профиль без custom style | Сохранить текущий штатный стиль и остальные параметры |
| Hive недоступен/состояние стиля не установлено | Остановить удаление SUT до системных изменений, показать причину |

Offline-reset должен согласованно учитывать ThemeManager (DllName/ColorName/SizeName), ссылку Themes/CurrentTheme и фактический `.theme` (VisualStyles), а также релевантные механизмы восстановления/синхронизации, выявленные тестами. Конкретный набор записей **не объявляется готовым** без Windows VM matrix. Не изменять оригинальные пользовательские файлы тем и не удалять обои/звуки/цвета; цель — безопасный штатный визуальный стиль. Не применять целую стандартную тему, если это без необходимости сбрасывает другую персонализацию.

### Транзакционная граница

1. Перечислить профили и сеансы, определить затронутые custom styles; проверить доступность всех профилей и достаточность прав.
2. Зафиксировать план по SID/сеансам и необходимые backup-данные целевых значений в защищённом журнале. Повторно проверить состояние сеансов перед изменением каждого hive.
3. Выполнить и проверить live/offline reset. Зафиксировать частичный результат по профилям при ошибке.
4. Если хотя бы один обязательный reset не подтверждён — системный SUT MSI не удалять. Отчёт должен признавать уже выполненный reset других профилей.
5. Только затем удалить SUT MSI с reboot suppressed. При MSI error не восстанавливать custom style автоматически, если работоспособность патчера не подтверждена. При 3010 сохранить pending.

Не откладывать весь reset до следующего входа после удаления SUT: иначе custom style может быть выбран до работы helper. Не полагаться на arbitrary RunOnce как доказательство безопасного порядка. Простое сканирование profiles не устраняет гонку нового logon; техническая синхронизация или безопасный отказ остаются частью реализации.

Проверки: Win10/11, локальные разные пользователи, активные/disconnected/logged-off сессии, повторный вход, перезагрузка, CurrentTheme указывает на custom theme, несовпадение CurrentTheme и ThemeManager, отсутствие .theme, синхронизация тем, HKCU другого администратора при UAC, MSI failure после reset. Реальные системные изменения выполнять только на VM.

## 5. Что ещё не доказано

- Точное происхождение каждого Windows/DWM значения без clean Windows baseline или registry trace.
- Полный минимальный offline-reset, безопасный на всех поддерживаемых Windows, и защита от concurrent logon.
- Владение и поведение конкретного SecureUxTheme MSI, миграция legacy и прямой uninstall в обход controller.

Это технические исследования, а не повторное согласование уже принятых пользовательских правил. Дамп/экспорт пользователя в репозиторий не скопирован; production-код не изменён.

#### Loaded SecureUxTheme removal: loaded-user registry keys only

The implementation now follows the explicit missing-key skip policy. Enumerate
ProfileList user entries; open existing `HKEY_USERS/<SID>` keys and pin their
handles for selector reset. If that key is absent, record SID and best-effort
ProfileImagePath, log the skip, and do not inspect/load/mount NTUSER.DAT. No local
account lookup or proof of account deletion is required. An absent key is not a
claim that the account is deleted. Access denied on an existing key remains an
error rather than silently pretending that it was processed.

Setup applies/verifies stock style in the current session, and writes/verifies
CurrentTheme and ThemeManager/DllName for the opened registry users only. Missing
selector subkeys inside an existing user key can be created. Skipped profiles are
not changed; each receives a separate finish warning after successful vendor MSI
removal, identifying folder (when available), SID, absent HKU key and unchanged
theme settings. Loaded removal retains its restart requirement. Other sessions
are not logged off or forcibly switched. Wallpaper/sound/color values and custom
theme/style files are untouched by these registry writes.

Rechecks verify opened user keys and processed selectors. If a previously absent
user key appears before the last check, require a fresh plan rather than apply
to an unexpected newly loaded profile. No global logon lock or atomic multi-profile
rollback is implemented. General optional profile cleanup retains its separate
strict offline inventory path and is not broadened by this SUT skip policy.

This supersedes the earlier deleted-local-account classifier and RegLoadKey
fallback; both have been removed from production, along with their obsolete
helper tests. No temporary global hive mounts or Backup/Restore privilege
changes are performed for SecureUxTheme removal.


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

### Зависимости действий Setup (новое правило планирования)

Зависимость требуется только для выбранного действия, а не для продукта вообще.
Если для удаления SecureUxTheme доступен более новый OS-native GlassSettings,
пользователь должен явно запланировать его обновление. Попытка удаления без
этого не переносит карточку; сверху показывается жёлтое уведомление.
План показывает фактический порядок: GlassSettings → удаление SecureUxTheme.
Отмена prerequisite отменяет зависимое удаление, но не выбирает продукты сама.
Разрядность определяется ОС; GlassSettings x86 не заменяет Native x64.

Если GlassSettings вообще не установлен, удаление разрешено с ответственностью
пользователя за предварительный возврат штатной темы и последствия удаления.
Карточка переносится, сверху появляется красное уведомление; предупреждение
закреплено в сводке предупреждений плана (полный текст — «Подробнее»), пока
удаление запланировано. Это явное исключение только из автоматической подготовки
стиля/Native-query; проверки оригинального MSI и его идентичности сохраняются.
Нельзя считать отсутствие или повреждение DLL отсутствием самого GlassSettings.

Уведомление о зависимости/риске сохраняется до первого следующего клика или прокрутки в
любой области окна, но минимум 1000 мс. Движение мыши, нажатие без
завершённого клика, перетаскивание карточки и клавиатура не учитываются.
Колесо мыши и прокрутка тачпадом учитываются, включая горизонтальную прокрутку.
Инициирующее событие не закрывает своё уведомление. Без следующего клика или прокрутки
оно не исчезает по таймеру. Новое уведомление начинает свой интервал заново.


Детали текущего контракта:
- Доступность обновления определяется по проверенному локальному MSI и/или
  скачиваемому предложению каталога: сравниваются версии GlassSettings отдельно
  для разрядности ОС. Повреждённый Native/отсутствие нужной разрядности при наличии
  другого GlassSettings также требуют явно выбранной установки/восстановления.
  Это консервативная зависимость от версии продукта, не сравнение отдельных DLL.
- Наличие только GlassSettings x86 на x64 — **не** отсутствие GlassSettings:
  необходимо явно запланировать GlassSettings x64. Отсутствие DLL, ошибка доверия
  или чтения MSI не разрешают ручное исключение.
- Отсутствие обеих MSI-регистраций проверяется повторно перед vendor removal;
  advertised/broken/неизвестные состояния регистрации не считаются отсутствием.
- При выбранной установке GlassSettings используется новый Native, а не ручное
  исключение. Нельзя незаметно заменить автоматическую подготовку на ручной риск
  отменой prerequisite; зависимое удаление отменяется, его нужно выбрать заново.
- После успешного удаления без Settings итоговая страница повторяет предупреждение
  о ручной подготовке и необходимости перезагрузки. Обои не являются критерием
  готовности; отсутствующие HKU по-прежнему не подключаются автоматически.


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

### Log17: parity с применением темы Settings

OS-native routing недостаточен без правильного apply pipeline. V2 подготовки
использует теперь ThemeManager2 (.theme) на STA, а не alias V1 с ordinal-only
применением .msstyles. HRESULT и точный текущий путь обязательны; одинаковый
путь при отказе API не разрешает MSI removal. V1 bitmask ABI сохранён.
См. GlassSettings/docs/stock-theme-maintenance.md. Оригинальный MSI, loaded-HKU
проверки, skip warnings и dependency/manual-risk политика остаются без изменений.


### Принудительное применение выбранной штатной темы (Log18)

После выбора aero.theme maintenance Native обязан вызвать GetCurrentTheme и
SetCurrentTheme(applyNow=1); только AddAndSelectTheme и stock path недостаточны
для этого протокола. Ошибки этих стадий запрещают vendor MSI. До завершения
помощника посылаются theme/system-color notifications для обновления окон.
Это не глобальная проверка отрисовки: Windows visual acceptance остаётся отдельной
проверкой, а state4/reboot означает лишь необходимость выгрузить загруженный SUT.


### Log19: проверка изменившегося Native при той же версии MSI

Log19 (20261109_145021) — vendor-only removal: rows0x0, glassSettings0,
Settings не обновлялся. Выполнен установленный Native x64, exit0; оригинальный
MSI SUT завершился3010, послеоперационное состояние4/reboot pending. Отсутствующие
HKU1008/1011/1012 пропущены как требуется. Пользователь сообщает, что активный
визуальный стиль не переключился. В логах нет fingerprint/version исполняемой
Native DLL и stage results, поэтому нельзя доказать ни наличие последнего
force-apply исправления, ни точную причину отсутствия видимого сброса.

Обнаружен пробел доставки action dependency: ProductVersion не идентифицирует
пересобранный Native внутри MSI с той же версией (здесь Setup2.0.0.0100).
Inspection теперь отдельно сравнивает установленный GlassSettings.Native.dll с
MsiFileHash для этого файла из **проверенного предлагаемого MSI**. Учитывается
правильный dwFileHashInfoSize; ошибка хеширования/отсутствующие или неоднозначные
метаданные означают unknown, не equality. Это сравнение свежести зависимости,
не переопределение integrity baseline старого установленного продукта.

При подтверждённом отличии OS-native Native требуется явно запланированная
переустановка/обновление соответствующего GlassSettings перед удалением SUT,
даже если ProductVersion тот же. Карточка сообщает «Native изменён · переустановить»,
без выдуманной стрелки обновления между одинаковыми версиями; попадает в needed
и рекомендации. Зависимость остаётся action-specific, не выбирается автоматически.
Неустановленный GlassSettings сохраняет прежнее manual-risk исключение.

В журнал добавлены compared/differs, ProductVersion обоих MSI и фактические/
предлагаемые MsiHash128. Это MSI content hash, **не SHA-256** и не самостоятельная
проверка доверия: подпись предлагаемого MSI и проверки installed helper сохранены.
Недоступное сравнение не заявляется доказательством совпадения. Remote MSI без
локально проверенного содержимого пока обслуживается по version policy.
Алгоритм переключения темы по одному Log19 повторно не менялся; visible Windows
acceptance по-прежнему отсутствует. Исходные загруженные логи удалены после анализа.


## Log20: недоступный hash и отсутствующая Release-диагностика

В запуске 20261109_145957 Settings64/32 installed/offered MSI=2.0.0.101,
но Native freshness compared=0/differs=0. Это unknown, не подтверждение совпадения.
План опять rows0x0/glassSettings0. В15:00:36.000 x64 helper exit0; vendor
MSI3010 в15:00:37.098, state4/restart1. Три missing HKU пропущены корректно.
Пользователь снова подтверждает отсутствие видимого переключения темы.

Исправлена ошибка предыдущего подхода: версионные PE-файлы обычно отсутствуют
в MsiFileHash; кроме того, существующий inner join может вернуть только прочие
неверсионные файлы MSI. Только этой таблицей нельзя обеспечивать свежесть Native.
Harvest теперь вызывает MsiGetFileHashW для **реального staged файла** с size=20
и сохраняет четыре DWORD в GLASSNATIVEMSIHASH внутри подписанного MSI. Для
Settings отсутствующая/неоднозначная Native или ошибка вычисления останавливает
сборку; старый табличный путь остаётся резервным для прежних пакетов. Плохое
свойство не заменяется старым hash и даёт unknown. При отсутствии метаданных
теперь есть явная строка причины, а не только compared=0. Политика явного
prerequisite, baseline integrity и ProductVersion не изменены.

Подробные HRESULT раньше шли через DebugLog, отключённый в Release. Добавлен
неавторизующий diagnostic mapping между Setup и OS-native helper: fixed-width
32-byte payload без указателей, локальное имя PID/tick, default same-user DACL,
отдельные результаты active/hr/COM/force/font/read/pathVerified. Setup пишет их
в обычный журнал после завершения процесса. Данные не заменяют exit code и
существующие проверки стиля/профилей. Нет записей на диск/в реестр. Старый Native
отклонит новый диагностический аргумент, и удаление остановится; требуется
установить пересобранный Native, а не обходить проверку. Отсутствующие результаты
явно логируются. pathVerified означает только совпавший путь, **не визуальный**
результат и не доказательство правильных цветов. Inactive no-op также различим.

Active query exit2 в log20 не подтверждает гипотезу inactive no-op; нет оснований
считать её причиной. Существующие profile preflight проверки сверяют desktop SID
с process token SID; запуск от другого UAC account также не установлен причиной.
Рендеринг/COM-алгоритм в этой итерации не менялся. Причина ложного визуального
успеха остаётся не установлена; новые stage results должны отделить реальное
выполнение COM/force от старой DLL/no-op и дать данные для следующего исправления.
Windows/MSVC/WiX-проверка недоступна; mock PASS не является visual acceptance.
Исходные log20 и vendor MSI log удалены после сохранения анализа.


### Log21: ANSI/Unicode dispatch в rundll32

Log21 (20261109_161115): Native source comparison теперь действительно сработал
(compared1/differs1 для обеих архитектур; installed MSI .101 → offered .103).
Явно выбраны обе GS: rows0x24; их MSI установились exit0 в16:12:41.030 и
16:12:47.985. Затем x64 style helper завершился **0x80070057 / E_INVALIDARG**
в16:12:48.469 без опубликованного trace. Vendor SUT MSI не запускался. Rollback
восстановил предыдущую установку (16:13:01.698). Missing HKU по-прежнему skipped.
Это не ошибка установки GS или vendor uninstall; новый diagnostic argument
выявил ошибку dispatch, которой предыдущие mock-тесты не моделировали.

Setup передавал rundll32 точное имя `GlassSettings_SecureUxThemePrepareRemovalV2W`.
Алгоритм rundll32 пробует requested+W как Unicode, requested+A и requested как
ANSI. Exact fallback не становится Unicode только оттого, что имя оканчивается
буквой W. Получался вызов LPWSTR callback с ANSI аргументом diagnostic mapping;
валидатор имени отклоняет такой буфер с E_INVALIDARG до применения темы. Раньше
пустой аргумент маскировал несовпадение сигнатуры. Совместимый алгоритм изучен в
https://github.com/wine-mirror/wine/blob/master/programs/rundll32/rundll32.c
(get_entry_point32); Windows machine-level trace здесь недоступен.

Исправлена только command line: requested base `...PrepareRemovalV2` без W.
Проверки PE/export/publisher/архитектуры остаются на реальном `...V2W`; LPWSTR
callback и строгая проверка имени mapping сохранены. Никаких новых версий
экспорта/искусственного обновления GS, bypass ошибки, fallback на x86 или
удаления hook вручную. Достаточно пересобрать Setup с этой правкой; существующие
пакеты с diagnostic Native должны быть установлены через явный prerequisite,
учитывая выполненный rollback. Модель lookup теперь проверяет actual production
команду и отдельно отвергает прежний exact-W ANSI dispatch.

Исправлен объяснимый новый отказ до применения темы. Это не подтверждение
устранения прежнего visible-style false success: после правильного вызова
ожидается строка stock-style stages с результатами COM/force/read, но видимое
переключение требует Windows acceptance. Исходные три журнала Log21 удалены
после фиксации анализа.


### Log22: rollback loop и неоднозначный HKU open

Session20261109_161821 повторно увидела installed GS MSI .101 / offered .103
и те же Native hashes, что Log21. Это согласуется с завершённым откатом Log21,
не с непрерывным успешным сохранением .103. Обе GS опять установились exit0
в16:19:13.803 и16:19:20.856; затем revalidation остановилась на SID1008:
RegOpenKeyEx HKU KEY_READ|KEY_WRITE вернул5. Style helper **не запускался**,
vendor SUT MSI также не запускался. В16:19:31.731 прежняя установка восстановлена.
Поэтому следующий запуск снова может законно предложить .103: отменять rollback
или скрывать действительно откатившуюся зависимость ради устранения badge нельзя.

Log22 не содержит namespace enumeration HKU и не доказывает, был ли1008 реально
перечисляемым loaded key с запрещённым чтением, отсутствующим ключом с отказом
доступа на open либо лишь корнем с запрещённой записью. Нельзя утверждать, что
это обязательно missing hive, или blanket-skip любой ERROR_ACCESS_DENIED.

Loaded-users-only inventory теперь сначала перечисляет имена непосредственно
HKU (read-only, без открытия SID). Отсутствующий SID пропускается с тем же finish
warning; неизвестный результат перечисления блокирует удаление. Перечисляемый
SID открывается только KEY_READ: изменения делаются в отдельно открываемых
дочерних theme/selector ключах, не в корне hive. Если ключ исчез между enum/open,
повторная enum должна подтвердить отсутствие до skip. Если он всё ещё существует
и недоступен для чтения — блокировка сохранена. Revalidation skipped SID также
использует namespace enumeration, а не open с неоднозначным результатом.
Не используются account deletion lookup, offline mount, unload, ACL rewrite,
privilege bypass или удаление профиля. General cleanup offline inventory не изменён.

Фактический profile_inventory mock проверяет absent+open-denied, present+denied,
root write denied/read allowed, enum failure, unload race, появление skipped SID
и повторную проверку; ASan/UBSan PASS. Это исправление inventory/probe, не доказанное
исправление рендеринга: последняя Windows-проверка вообще не достигла theme apply.
Три исходных журнала Log22 удалены после сохранения выводов.


### Log23: live style подтверждён; создание отсутствующих selector keys

Session20261109_162939: обе GS .103 установились exit0 (16:30:22.120,
16:30:29.010). В16:30:41.708 x64 style helper exit0, trace active1,
hr/COM/force/font/read=0, pathVerified1. Пользователь **подтвердил видимое
переключение стиля**. Это первая проверка, где есть и stage trace, и визуальное
подтверждение; не следует снова менять работающий COM-путь.

Следующий этап завершился отказом записи CurrentTheme для1008 (код5).
Vendor MSI не запускался, GS откатились (16:30:53.511). Прежняя диагностика
объединяла create/set/readback и не доказывает, какой вызов вернул ошибку.
Но в коде найдено несоответствие: preflight разрешает ERROR_FILE_NOT_FOUND для
selector key, commit безусловно RegCreateKeyEx, а verify требовал созданный ключ.
Отсутствие theme key не означает наличие custom-ссылки и не даёт оснований
создавать персонализацию удерживаемого профиля удалённого пользователя.

Теперь existing selector key открывается KEY_QUERY_VALUE|KEY_SET_VALUE в
preflight/commit; никаких RegCreateKeyEx на этом пути. Missing key пропускается
и при проверке, с явным no-key-created log; остальные существующие ключи/ссылки
всё равно заменяются и проверяются. ERROR_ACCESS_DENIED для существующего ключа
не игнорируется. Readback использует KEY_QUERY_VALUE. Запись значения в existing
key по-прежнему проверяется; отсутствие самого hive обрабатывается отдельной
inventory policy. General cleanup и live style helper не изменялись, offline
mount/unload, ACL bypass и account deletion proof не добавлялись.

В error detail теперь stage=open/set/readback, чтобы не выдавать оставшиеся
ошибки прав доступа за проблемы создания ключа без доказательств. Предположение,
что именно create дало5 в Log23, остаётся выводом по коду, а не API trace.
Mock исполняет реальные методы: absent theme keys не создаются и не записываются,
смешанные existing/absent профили, минимум прав, ошибки open/set/readback,
проверка существующих stock-ссылок и one-shot commit. Windows/MSVC/WiX acceptance
этой правки не выполнялся. Исходные три журнала Log23 удалены после анализа.


### Log24: SUT removal успешен; post-commit repair-source warning

Session20261109_163708: GS64/32 установлены exit0; stock helper exit0;
пользователь подтвердил успешное переключение и удаление. Vendor SUT MSI3010,
after-state4/restart1 — требуется перезагрузка. Это успешная основная операция,
не rollback и не сбой старта компонентов.

Затем Phase C LaunchSelectedProducts вызвала maintenance Publish, потому что
в плане были package apply GS (rows0x24). Реализация берёт acquired GS sources,
но для неизменённых продуктов переносит sources из прежнего Current. Setup и
Unins были скопированы/extracted в Staging; hardened stage активирован как Current.
Регистрация repair sources для Current/Glass-x64.msi и Glass-x86.msi отказала:
ValidateRepairSourceDigest, Policy13, no verified distribution/installed-cache
binding; legacy source is not registered automatically. Это отказ политики
проверки, не access denied. Для повторно использованных legacy MSI нет принятой
проверяемой привязки distribution SHA256 + installed LocalPackage SHA256.
Одна подпись/версия или PackageCode не заменяет эту привязку.

Код RegisterRepairSource сначала сопоставляет version/PackageCode регистрации,
затем проверяет PublishedDigestMatches, и только затем меняет MSI SourceList.
В Log24 source registration отказала после activate-current; предыдущий комплект
не освобождён из-за ошибки. Поэтому общее сообщение «оболочка сохранена не
полностью» не означает, что Setup/Unins не скопированы или что Current вообще
не появился. Работоспособность launcher отдельно этим журналом не проверена.
Не заявляем точное отсутствие sidecar: binding может отсутствовать/не совпадать.

На запрос пользователя новая runtime политика НЕ внедряется. Общие ограничения
актуальный controller + только необходимые старые sources ранее записаны, но
детальная публикация оболочки при частичном составе и legacy migration требуют
отдельного согласования. Предложение (не утверждено): отделить состояние controller
от покрытия offline repair по фактически установленным products; publish latest
controller независимо от изменения платформы, сохранять только доверенные exact
sources оставшихся продуктов; unknown legacy coverage не превращать в доверенный
source и явно показывать ограничение конкретного продукта, отдельно от ошибки
сохранения оболочки. Ограничить Previous/recovery retention подтверждёнными pins,
не держать полные исторические комплекты постоянно. Конкретная migration/GC
реализация, UI статусы и границы atomicity этим обсуждением не утверждены.
Исходные Log24 и три MSI-лога удалены после фиксации анализа. Runtime code не менялся.
