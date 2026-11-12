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
- Ставится только подкаталог `Resources` найденной Inno установки. MSI можно
  направить через `OPENGLASSROOT` после проверки каталога в Setup (интеграция впереди).
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
