# Задание Cursor: почему Glass Apply недоступен на новом ПК

Нужен **диагностический отчёт**, не автоматическое исправление системы.
Не устанавливать/удалять продукты, не запускать MSI/OG, не импортировать сертификаты,
не править registry/cache/ACL и не отключать trust checks. Особенно не запускать
OpenGlass 3.0.2.3749 на Windows 11 26H2 26300 ради воспроизведения.

## Собрать факты

1. Commit исходников и путь/версию реально запущенного GlassSetup.exe, Windows
   DisplayVersion/CurrentBuild/UBR, архитектуру OS. Если это новый build, указать
   его commit; одного FileVersion недостаточно.
2. SHA-256 и Authenticode запущенного Setup: Status/StatusMessage и signer thumbprint.
   Не запрашивать/экспортировать private keys, токены и сертификатные хранилища.
3. Запустить UI только до Components (или взять предыдущий отказ), **не нажимать
   Apply/Install**. Снять свежий Setup log и screenshot карточек с версиями/источниками.
   Если случился controller handoff, нужны оба журнала и пути обоих EXE.
4. Скачать **только version.json** по URL
   `https://github.com/musicstashall/GlassPlatform/releases/latest/download/version.json`.
   Сохранить исходные байты и указать итоговый redirect URL, HTTP status/ошибку.
   Это обязательная локальная проверка: агентская среда получает release metadata,
   но скачивание release asset здесь обрывается на внешнем asset-host.
5. Для platform64/platform32/settings64/settings32/openglassResources вывести
   `id`, `file`, `version`, `url`, `size`, длину/значение SHA-256. Проверить:
   - products — массив, без duplicate IDs;
   - version — числовая MSI ProductVersion, не DLL version;
   - filename совпадает с runtime kFiles;
   - size — целое положительное >=512, SHA-256 — 64 lowercase hex;
   - URL вида `https://github.com/musicstashall/GlassPlatform/releases/download/<tag>/<file>`;
   - setup и setupPackage.version совпадают.
   Не менять production manifest; просто перечислить нарушения schema.
6. Для уже имеющихся рядом с Setup/в `packages` MSI перечислить path/size/hash,
   подпись и read-only MSI metadata (ProductCode/ProductVersion/architecture).
   Не скачивать/запускать MSI, если их нет. Наличие локального файла ещё не означает доверие.
7. В свежем логе искать:
   - catalog fetch/parse failure, `Parent catalog check was unavailable`;
   - `Local MSI rejected`, signer/trust/hash/identity/version errors;
   - новые `MSI offer selected/unavailable/discovery failed`, `UI catalog source`;
   - различие `version` и `verifiedLocal=0` — для server offer нормально;
   - наличие targetVersion при false applySupported в отправляемом state, если есть debug instrumentation.
   При необходимости read-only debugger/trace поставить на ComponentsJson: выписать
   rep.targetVersion, rep.sourceVerified, releaseCatalog entry/downloadable,
   installedPackageVersion и applySupported для каждой заблокированной карточки.
   Не менять значения в debugger, не обходить gate.

## Вернуть отчёт

- ОС, commit/FileVersion/пути parent+controller, screenshot.
- Оба Setup logs и исходный version.json (без персональных секретов).
- Таблица по продуктам: installed / discovered version / verifiedLocal /
  catalog entry+downloadable / applySupported / конкретная причина отказа.
- Разделить доказанные факты и гипотезы. Не писать «сеть», «версия совпадает» или
  «сертификат» без соответствующего error/status из журнала.

Исправление в текущей ветке: UI больше не отбрасывает rep.targetVersion только
из-за verifiedLocal=false; это **не** разрешение выполнять непроверенный MSI.
Окончательный acquisition по-прежнему проверяет hash, подпись, identity и downgrade.
