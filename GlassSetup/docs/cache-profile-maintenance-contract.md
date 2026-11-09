# Кэш, очистка профилей и сброс визуальных стилей

Дата: 2026-11-04. Уточнение после согласования редакции 2 таблицы состояний. Это проект реализации, не выполненная очистка Windows.

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

#### Loaded SecureUxTheme removal: all-profile selectors

Loaded removal inventories all ProfileList user profiles and pins their loaded
or privately opened offline hives. It no longer requires exactly one profile,
matching elevated/desktop SIDs or an absence of other sessions. Before the original
MSI, Setup switches the current session itself through the shared
ThemeHelper::SetSystemVisualStyle implementation (no installed V2 export);
Setup then writes/verifies each profile's `Themes/CurrentTheme` to Windows
`Resources/Themes/aero.theme` and `ThemeManager/DllName` to Windows
`Resources/Themes/Aero/aero.msstyles`. Missing selector keys can be created.
Preflight does not change registry values. Stock file existence/non-reparse checks
remain; Authenticode checks are not required for this reset.

Wallpaper/sound/color settings and custom theme/style files are not modified by
these writes. Other active sessions are not forcibly switched or logged off:
their persisted selectors are prepared for the next logon/restart. Loaded removal
retains its reboot requirement. Profile inventory and written selectors are
rechecked before the vendor MSI. An inaccessible profile or failed write/readback
blocks that MSI and reports the profile SID/value; earlier style/selector changes
can remain. There is no automatic custom-style restore or atomic multi-profile
rollback. Concurrent profile creation/theme changes are not locked globally.

Runtime queries still use the installed OS-native Settings helper, so its
same-plan removal is still refused. Stock-style preparation itself no longer
requires a particular installed Settings version or any V2 export. Restrictions
on profile count or other sessions remain removed.

For SecureUxTheme style preparation only, confirmed deleted **local SAM account**
residues in ProfileList are not live user profiles: compare the account SID prefix
to LSA PolicyAccountDomainInformation and require ERROR_NONE_MAPPED from account
lookup before ignoring that entry. Recheck classification with inventory before
commit. A foreign-domain/cloud SID or any other lookup failure is not deletion
evidence. Ignore residues before attempting NTUSER.DAT; do not unload system hives,
repair/delete profile data or make general settings cleanup skip such entries.

SUT removal's offline loader now falls back from RegLoadAppKey ERROR_BADDB=1009
to RegLoadKey under a GUID-named HKU GlassSetup-Offline mount with scoped
Backup/Restore privileges. Only successful loads confer mount ownership; existing
SID mounts are never unloaded. Profile handle/move/destructor lifetime owns close
and private-name unmount. Both loader failures stop preparation and report codes.
This is the normal Windows loader, not custom hive repair. Mount cleanup failures
are logged; crash recovery of owned mounts is not yet journaled. General settings
cleanup remains on its original application-hive loader path.
