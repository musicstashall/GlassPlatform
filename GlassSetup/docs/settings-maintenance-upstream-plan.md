# Settings: обслуживание, резервная копия, обновления и upstream OpenGlass

## Новый запрос пользователя

Статус: этап анализа, не готовая реализация. Онлайн-установка ранее принята;
новая упаковка/backup/обновления потребуют собственной приёмки.

1. Оформить страницу full-removal cleanup в общем стиле: компактная иерархия,
   читаемые абзацы/группы, без длинного размазанного блока описания; HTML/native RU/EN.
2. Удалить строку «Обслуживание установки» с Home. На странице «Системы» добавить
   спойлер «Обслуживание системы», справа кнопку «Изменить» → trusted cached Setup.
3. Здесь же кнопка «Сохранить копию параметров». Назначение по умолчанию — корень
   Documents пользователя. Не добавлять backup UI в Setup вместо этого требования.
   Архив должен быть проверяемым, с manifest/version/checksums и исходными типами/
   байтами registry values. Scope — область согласованной очистки Glass/DWM;
   не экспортировать целый DWM, hive NTUSER.DAT, сертификаты или MSI cache.
   Для экспортируемых файлов запрещены reparse traversal и небезопасная elevated
   запись в пользовательский путь. Ошибки/неполный архив явно сообщаются.
4. Все кнопки «Обновить» запускают cached Setup. Проверка Glass — отдельно по
   установленным продуктам из release catalog, не bundle field `glass`. Несколько
   обновлений: обобщённая строка/notice на спойлере «Платформа Glass», не длинный
   список. OpenGlass notice остаётся на своём спойлере.
5. OpenGlass updates — только для установленной DWM architecture (Legacy/MILComp).
   Переключение канала из-за появления другого тега не допускается.
6. Не менять поведение отсутствующей Platform x86. Подпись split — «Отключена».
7. Найти регрессию удаления OpenGlass service/task тумблером. Dropdown Delete и
   switch должны согласованно удалять оркестрацию, а не только disable autostart.
8. Графические дополнения Resources выделить в отдельный пакет с зависимостью
   от OpenGlass; учесть install/update/remove/snapshot/rollback/catalog/UI.
9. Собственный OpenGlass MSI отменить. Загружать upstream release, извлекать
   только нужный OpenGlass.dll по Windows/DWM architecture. Сначала определить
   servicing/receipt/source trust и переход с ранее установленного нашего MSI,
   Не сохранять старое MSI семейство или ветку миграции (контракт ниже).
   Не запускать upstream EXE install.

## Результат первичной проверки

- `GlassSettings/Controls/PlatformSchedulerRow.cs`: отсутствующая x86 использовала
  Status_PlatformNotInstalled. Исправлен только текст на Status_Disabled;
  locks/enabled/action logic неизменны. Source regression guard добавлен.
- OpenGlass switch и dropdown сходятся в ApplyInstallToggleAsync(false). Switch
  дополнительно сравнивает QueryDirectState().TaskExists с новым toggle value.
  Причина регрессии пока не доказана; нельзя объявлять исправленной без проверки
  service-first query/cache/event sequencing и runtime сценария.
- `GlassReleaseUpdateService` ещё читает bundle `glass`, OpenGlass provider ищет
  только `-legacy`. Это не соответствует новому требованию.
- После первого анализа получены новые remote commits обычным fetch/merge,
  включая `GlassSetup/docs/openglass-upstream-setup.md`. Это нормативный контракт:
  собственный parser Inno 6.7, проверка publisher, CRC/SHA256, bounded LZMA и
  CALL/JMP decode по 64KB; PE/version/signature проверка каждого файла.
  Старое MSI семейство удаляется без migration/compatibility ветки.
  В соответствии с инструкцией устанавливаются выбранная DLL + Host + GUI,
  а не DLL без host. Не перечитывать EXE вместо предоставленного формата.
- Upstream API Releases: v3.0.2.3749/v3.0.1.3747/v3.0.0.3733 публикуют
  OpenGlassSetup.Signed.exe и symbols; старые v3.0.0.3329-milcomp /
  v2.6.2.2891-legacy имеют раздельные теги. Нельзя предполагать ZIP payload.
- Проверенный текущий upstream Scripts/OpenGlass.iss содержит оба DLL в EXE;
  selector Windows build >=28000 → MILComp, иначе Legacy. Современные unsuffixed
  релизы нельзя игнорировать при реализации проверки обновлений.
- Host/GUI сохраняются согласно полученной инструкции, служба управляется
  существующим кодом Glass. Независимый upstream Inno install не перезаписывать.
- Пришедший remote commit e3ab778 возвращает GlassSettings на общую bundle version;
  не восстанавливать удалённый Version.props при per-product update реализации.
- Кнопки update в ProductUpdateCoordinator уже вызывают cached Setup. Требуется
  сохранить этот маршрут, а не создавать новый downloader внутри Settings.

## Порядок реализации

A. Подтвердить upstream contract по инструкции, определить состав и владение
   Resources/OpenGlass, формат экспорта и consumers registry на новой версии.
B. Settings maintenance UI/backup + исправления orchestration и cleanup page.
C. Per-product update provider/compact notices/cached Setup launch.
D. Новый OpenGlass source/provider + отдельный Resources package, планировщик,
   rollback/uninstall/migration; обновить release generation и тестовую матрицу.
E. Windows build/VM acceptance, включая backup перед destructive cleanup.

Текущий малый UI fix: GlassSettings Release|x86 и Release|x64, зависимости уже
собранные GlassShared/Native соответствующей архитектуры. Другие этапы пока
не реализованы; порядок сборки потребуется уточнить после изменения пакетов.
