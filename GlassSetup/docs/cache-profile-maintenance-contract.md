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
