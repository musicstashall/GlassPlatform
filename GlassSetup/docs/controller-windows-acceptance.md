# Controller lifecycle: следующий шаг — проверка на Windows

Статус: пользователь собрал и успешно проверил online startup без операций:
protected controller handoff, inherited catalog, один product inspection и UI без
нареканий подтверждены журналами и наблюдением пользователя. Дополнительно приняты
offline startup (с bounded failed network attempts) и ARP modify entry без MSI.
Вход из обновлённого GlassSettings подтверждён ticket authorization. Повторное
обновление Settings x64/x86 и Platform x64/x86: uninstall/install code0, module
gate clear без Retry, source publication completed. Happy-path MSI принят; полный
Authenticode/ACL fault matrix, reboot/crash recovery не выполнены. Разбор общей остановки
и падений Explorer по решению пользователя вынесен в отдельную тему; это не
подтверждение исправления и не задача текущего прохода установщика. Подробнее — начало implementation-progress.md.
Совместимость со старым Settings entrypoint не добавляется. Не использовать рабочую
машину для fault-injection или намеренного повреждения cache.

## 1. Сначала сборка, без установки

Из корня solution в VS 2022 Developer Command Prompt, с обычными зависимостями
проекта (v143/Windows SDK/.NET 9/WiX/NuGet):

Собирать Release prerequisites обеих архитектур по [README](../README.md).
Resources MSI — **только Release|x64** (`msi/OpenGlassResources-x64.wixproj`),
не выбирать его в solution Release|x86. Затем актуальные GlassUnins → GlassSetup
(C++ Release|Win32 / solution target x86). Полный состав/публикация —
[release-checklist.md](release-checklist.md). Безусловный build всего solution x86
для Resources не применять.

Порядок x64 → x86 сохраняется; итоговый Setup —
`Build\GlassSetup\Release\GlassSetup.exe`. Следовать текущему README, не собирать
отдельный Unins до необходимых Native helpers. Нельзя объявлять сборку успешной
только по наличию EXE от предыдущей ревизии. При C++/C#/link/WiX ошибке остановить
проверку и передать первый error с контекстом/полный build log.

LNK1168 — не повод агенту завершать процессы/останавливать платформу. Действия
с работающей системой выполняет только пользователь.

## 2. Первая короткая проверка

1. Использовать подписанный комплект с проверенным сертификатом автора, в том числе
   на чистой VM **без** доверенного root. Controller требует совместимую PE-архитектуру,
   точное совпадение signer с запущенным Setup и WinVerifyTrust success либо только
   `CERT_E_UNTRUSTEDROOT` (0x800B0109). Все прочие ошибки блокируют запуск. Импорт root
   для публикации контроллера не требуется. При ином trust error передать журнал;
   не отключать проверку, не обходить её переименованием или правкой Active.
2. Запустить Setup из обычного distribution layout. После Checking новый процесс
   должен работать из `%ProgramData%\Packages\GlassPlatform\Installer\<SHA256>`.
   Не должно быть бесконечного relaunch, двух активных мастеров или автозапуска MSI.
3. Убедиться, что до WebView не вспыхивает отдельный native Checking. После
   relaunch должен отображаться анализ продуктов, не повторный поиск обновления
   Setup. В двух журналах цепочки ожидается один запрос version.json и один полный
   MSI-source inspection (только в конечном controller).
4. Дойти до выбора продуктов и отменить. Установленные продукты/стили/профили не
   должны измениться только из-за обновления controller.
5. Повторить запуск с отключённой сетью: доверенный installed controller должен
   позволять обслуживание без обязательного скачивания. Доступность самих MSI —
   отдельный вопрос, отсутствие original source не маскировать как controller failure.
6. Проверить запуск обслуживания из Settings и зарегистрированных ARP-команд.
   Новый Active должен использоваться без предварительного обновления всех продуктов.

Для диагностики нужны: build result, Windows/OS architecture, точное действие,
ожидаемое/фактическое поведение и Setup/Unins log. Скриншот — если ошибка в UI.
Не нужны ручные правки реестра или удаление `%WINDIR%\Installer`.

## 3. Расширенная матрица — отдельная VM со snapshot

| Сценарий | Ожидаемый результат |
|---|---|
| Старый external Setup, более новый Active | Не понижает Active; запускает installed controller |
| Равная версия, разные файлы | Совместимый installed controller имеет приоритет; v1/v2 мигрирует на v3 без понижения версии |
| Старый parent запускает новый v3 child | Сохраняется initial COMMIT barrier, startup подтверждается самим child; не зависает в ожидании отсутствующего второго ACK |
| Новая версия controller, продукты без изменений | Обновляются только Setup/Unins и entrypoints |
| Неверная подпись, несовместимый PE, подмена digest/record | Отказ до MSI, нет запуска download workspace |
| Pending до swap Active | Прежний Active остаётся; stale intent не становится authority |
| Падение после Active, до startup ACK | При следующем запуске проверенный Confirmed восстанавливается; плохой candidate не вытесняет его повторно |
| Первая публикация без Confirmed | Только проверенная forward-попытка; не заявляется невозможный rollback |
| Падение после Confirmed, до удаления Pending | Подтверждённый Active сохраняется |
| READY есть, startup не завершён/процесс умер | Bounded timeout, нет продуктовых действий child |
| Два запуска/другая сессия, retirement во время Setup | Lease/locks не позволяют удалить работающий controller |
| Старые файлы заняты | CleanupPending, без kill; повтор после закрытия старого launcher |
| Current содержит MSI продукта вне нового плана | Нужный exact original попадает в content-addressed Retained |
| MSI с одинаковой identity, но другими distribution bytes | Не подменяет exact original по одному PackageCode/version |
| Legacy MSI без binding | Сохранение как candidate, NeedsOriginalSource; нет автоматического доверия |
| Неизвестные SID/context/LocalPackage | Source не удаляется |
| Частичный removal | Sources оставшихся продуктов и controller сохраняются |
| MSI 3010/1641 или SUT restart | Reboot pin удерживает sources/controller до reboot |
| После reboot | Volatile pin исчезает; следующая безопасная GC/retirement может повториться |
| Последний product removal без reboot | Parent пытается retire; locked parent EXE может остаться inert CleanupPending |
| Ошибка product MSI после controller update | Controller result не откатывается вместе с продуктом |
| Ошибка cleanup после product commit | Не переименовывается в product failure и не откатывает MSI |

Fault-injection — только на snapshot VM. Не завершать рабочую платформу ради этих
тестов. Не загружать/выгружать пользовательские NTUSER.DAT и не удалять аккаунты.

## 4. Повтор удаления только неиспользуемого controller

Когда продуктов уже нет и окна Setup/Unins закрыты, существующий доверенный
`GlassUnins.exe --remove-controller` выполняет отдельную попытку cleanup. Команда
не удаляет продукты/профили и отказывается при registrations, неизвестном состоянии,
активном lease или reboot pin. Exit 0 — завершено; ERROR_BUSY (170) — cleanup не
завершён/не разрешён. Запуск из удаляемого каталога может оставить собственный
занятый EXE; повтор из подписанного distribution Unins после закрытия старых
launchers убирает остаток. Не требуется вручную удалять Active или системный cache.

Online/offline startup и Settings/ARP entry ранее приняты в happy path (см. начало).
Повтор для новых trust/removal изменений и полная fault/reboot/crash acceptance
остаются открытыми; прежний happy path не закрывает новые сценарии.


## 4. Не закрытые последними mock-прогонами сценарии

- Setup 273: shell unregister helper timeout 45 s, затем sharing violation на
  GlassShellExt.dll при rollback. Повторить после pre-MSI barrier/no-wait изменений;
  точная внутренняя причина hang пока не доказана. Не считать остановленные задачи/OG причиной.
- Чистый ПК: первый self-signed controller, embedded Unins и parent verification.
- OG removal: unquoted spaced ImagePath с trailing whitespace, running-image match,
  ambiguous prefix EXE refusal, service 1072 vs timeout/access denial. Никаких probes,
  принудительного удаления службы, смены ACL, kill или автоматического reboot.
- Installed Apps: all-context отсутствие MSI, ключи обеих views отсутствуют,
  уже открытое окно обновляется без активных stale Modify/Uninstall.
- Состав: пять first-party MSI (Resources только x64), официальный OG EXE и
  upstream SUT отдельно; Resources перед OG при fresh pair и servicing без OG.
- Partial mixed result и сохранение recovery snapshot при неполном rollback;
  .glassbackup restore/error/disk-full отдельно от installer snapshot.

Portable mocks и source smoke tests не закрывают ни один из этих Windows gates.
