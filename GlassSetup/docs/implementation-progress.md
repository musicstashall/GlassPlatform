## Исправление графа сборки SUT MSI

SecureUxThemeCa включён в solution с Release x64/Win32 mappings и зависимостью от GlassShared; оба WiX-проекта зависят от CA. Вложенный MSBuild CA отключён при сборке solution/Visual Studio, чтобы не пересобирать общую библиотеку параллельно планировщику solution. Прямой MSBuild отдельного WiX-проекта сохраняет внутреннюю сборку CA. Добавлена проверка GSUT001 и уточнены сообщения проверки отсутствующего MSI. Порядок Release x64 → Release x86 неизменен. `tests/verify_sut_build_graph.py` проверяет граф, конфигурации, отсутствие циклов и рекурсивной solution-сборки; это статическая проверка, не доказательство успешной MSVC/WiX сборки и не подтверждение единственной причины пустых каталогов пользователя.

# Правки обслуживания после аудита 233b21c — 2026-11-05

**Рабочая публикация для проверки сборки и диагностики в изолированной Windows VM. Полное исправление обслуживания ещё не завершено; это не готовый выпуск.**

В рабочем дереве исправлены: преобразование MSI errors в HRESULT для GlassSettings (положительный 1603 больше не считается успехом); перепроверка регистрации после MSI removal; машинное чтение/запись install-path в Setup и основных managed/native читателях GlassSettings; машинный snapshot путей rollback; установленный Native helper вместо извлечения в GlassUnlocker; отсутствие SUT style preflight/query при отсутствии Settings в Unins; допустимый ABSENT component в MSI path inspection; обязательный выходной буфер MsiEnumProductsEx; экспорты CA DLL; deferred вместо commit CA и явные условия REMOVE=ALL; отключены старые native entry points ручного удаления системных файлов/регистрации; убрана тавтология OpenGlass. Исправлен mock защищённого child ACL и устаревшие source assertions из предыдущей реализации.

Проверено: resource/source smoke, 4096 portable planner cases, cache diagnostics mock; Windows GNU object compilation InstallUtil/InstallMsi/InstallRollback/InstallEngine и SUT CA x86. Это не MSVC/WiX и не Windows runtime.

**До завершения остаются блокирующие части:**
- Реальный maintenance plan с явными действиями Keep/Install/Repair/Update/Remove и единым выполнением GUI/CLI; сейчас Setup по-прежнему не умеет удалять.
- Проверка системного SUT MSI: текущий generic inspector всё ещё запрещает CustomAction и System32 за пределами install root. Нельзя обходить это без проверки доверия и схемы пакета.
- Автономные stop/delete собственной службы и задач OpenGlass в MSI, обработка оставшейся отключённой службы; сейчас этот сценарий ещё не исправлен.
- Транзакционные snapshot/rollback IFEO/COM для SUT CA и сохранение чужих значений/токенов; изменение Execute на deferred само по себе этого не завершает.
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
2. **SecureUxTheme (2026-11):** per-machine MSI в System32/SysWOW64; IFEO/COM — custom action `SecureUxThemeCa`; Glass Platform / GlassSettings / SecureUxTheme — `Scope="perMachine"` (миграция: uninstall per-user, затем install per-machine на том же ProductCode). Кэш ProgramData не трогает ACL общего `Packages`. GlassSettings: тумблер ставит/снимает MSI из кэша; без платформы скрыты разделы Glass/текст/процессы. Страница сертификата Setup — только при выбранной платформе.
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
