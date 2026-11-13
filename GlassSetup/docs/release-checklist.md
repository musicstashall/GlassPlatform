# Ручной release checklist установщика

Актуальный состав после installer stage PR #8. Это список действий, не отчёт об
их прохождении. Результаты Windows записывать отдельно с commit/build, версиями
Windows и продуктов, Setup/MSI/Inno logs. Не публиковать старые MSI с новым catalog.

## 1. Сборка и состав

- [ ] Зафиксировать commit, версию bundle и версии каждого MSI/Setup. Очистить
  неоднозначность старых артефактов: наличие EXE от прошлого build не означает успех.
- [ ] Собрать необходимые Release Native/platform/Settings x64 и x86, shell DLL/host/MSIX.
  Использовать зависимости и команды из [README](../README.md). При первой ошибке остановиться.
- [ ] **Resources — только Release|x64**: `GlassSetup/msi/OpenGlassResources-x64.wixproj`.
  Никогда не выбирать этот проект в solution Release|x86. Проверить graphics-only staging.
- [ ] После обеих архитектур helpers собрать GlassUnins, затем GlassSetup
  (C++ проекты Release|Win32, solution-target mapping x86). Упаковывается актуальный Unins.
- [ ] В `Build/GlassSetup/Release`: `GlassSetup.exe`, `Glass-x64.msi`, `Glass-x86.msi`,
  `GlassSettings-x64.msi`, `GlassSettings-x86.msi`, `OpenGlassResources-x64.msi`.
  Пять first-party MSI, не шесть; собственного OpenGlass MSI больше нет.
- [ ] OG — отдельный официальный `OpenGlassSetup.Signed.exe` из upstream release;
  SUT — отдельный официальный upstream MSI. Не переподписывать и не включать их
  в first-party product catalog. VC++/WebView2 prerequisites — отдельные источники.

## 2. Подпись и portable regression

- [ ] Подписать актуальные first-party PE, Unins, Setup, shell MSIX и все пять MSI.
  Выполнить `Shared/scripts/Test-GlassSignatures.ps1`; дополнительно убедиться,
  что **Resources MSI** тоже проверен (старый список скрипта описывает четыре MSI).
- [ ] Проверить совпадение сертификата с [docs/signing.md](../../docs/signing.md),
  целостность подписей и срок действия. Не считать один CN доказательством.
- [ ] Self-signed first-party controller/MSI: точный signer запущенного Setup +
  WinVerifyTrust success или **только** `CERT_E_UNTRUSTEDROOT` (0x800B0109).
  Отсутствие подписи, bad digest, другой signer, chaining/expiry/revocation запрещены.
  Для этой проверки не импортировать root и не отключать trust gates.
- [ ] Не смешивать политику Glass с upstream: скачанный OG EXE требует полноценной
  доверенной подписи ожидаемого publisher. Ограниченное unsigned-исключение относится
  только к защищённому зарегистрированному установленному Inno-деинсталлятору;
  оно не распространяется на downloads или повреждённые подписанные файлы.
- [ ] `python3 GlassSetup/tests/run_mocks.py` — все portable mocks, отчёт и логи,
  ненулевой exit при failure/timeout. Затем `python3 GlassSetup/tests/verify_resources.py`
  и `git diff --check`. Browser/DOM tests выполнить отдельно в подходящей среде.
  Это не MSVC/WiX/Windows acceptance.

## 3. Catalog и публикация

Порядок загрузки, частичное обновление и заметки релиза —
[release-publish.md](release-publish.md). `version.json` заливается последним.

- [ ] Сгенерировать catalog **из финальных подписанных файлов одного build**:

```powershell
.\GlassSetup\scripts\New-ReleaseCatalog.ps1 -ReleaseTag <release-tag> -IncludeSetup `
  -Products platform64,platform32,settings64,settings32,openglassResources
```

- [ ] Проверить пять product entries, Setup version, точные filenames, SHA-256,
  размеры и allowlisted URLs. Resources нужно указать явно: default Products — четыре.
  Не брать version.json от нового build, а MSI от предыдущего.
- [ ] Загрузить пять first-party MSI и Setup, затем version.json последним.
  Для частичной публикации применять `-BaseManifest`, не терять остальные entries.
  MSI, чей SHA-256 уже совпадает с релизом, повторно не заливать.
- [ ] Скачать опубликованные файлы и заново сверить bytes/hash/version с catalog.
  Проверить adjacent/cache/release resolution без подмены local version под remote.

## 4. Windows gate перед объявлением готовности

- [ ] Чистая VM **без доверия self-signed root**: initial controller publication,
  равные server/local версии, online и offline запуск; нет цикла/двух окон/авто-MSI.
- [ ] Обновление контроллера, Modify из Installed Apps и Settings, сохранение позиции
  перемещённого окна, x64/x86 maintenance, отказ чужому/повреждённому controller.
- [ ] На 26300 выбранный OG 3.0.2.3749: красная карточка/значок/предупреждение;
  другие builds/версии и removal не получают ложный compatibility risk.
  Accept all не выбирает OG/Resources, ручной выбор остаётся.
- [ ] Fresh OG+Resources: Resources **до** запуска официального EXE. Resources
  maintenance/removal работает и без OG. Одна папка OG не считается installation.
- [ ] Removal keep/clean, обе архитектуры: shell-unregister barrier **до первого MSI**;
  воспроизвести последний 45 s hang. No-wait notification — не доказательство исправления.
- [ ] OG: explicit stop непосредственно перед Inno, подтверждение stopped и закрытие
  handles. Отдельно 1072, timeout/access denied; при неуспехе Inno не запускать.
  Default flags `/DELETEPRESETS /NODELETECONFIG`; согласие OG cleanup убирает
  `/NODELETECONFIG`, Platform cleanup независимо. Без принудительного reboot/kill.
- [ ] После MSI removal проверить отсутствие all-context MSI и ARP в обеих views,
  немедленное обновление **уже открытого** Installed Apps, отсутствие рабочих stale
  Modify/Uninstall. Не выдавать broadcast за доказательство обновления Settings.
- [ ] Ошибка/отмена mixed plan: честное partial состояние, сохранённый recovery path;
  успешный rollback не объявлять при sharing violation. Не удалять failed snapshot.
- [ ] Расширенный fault/reboot/ACL matrix:
  [controller-windows-acceptance.md](controller-windows-acceptance.md), только VM.

Доверие для compact shell MSIX — отдельное явное согласие на TrustedPeople.
Controller self-signed исключение его не подразумевает, root store не меняет.
