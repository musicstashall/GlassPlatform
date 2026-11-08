# Setup: карта процедур по явным действиям

Keep — отсутствие мутации, а не разрешение обслужить зависимость. Install/Update/Repair
исполняются через Apply; Remove — отдельное действие. Карта — объединение строк запроса;
она не добавляет установку/удаление других продуктов. Решение выполняется до Stop.
Авторизацию MSI/пути/контекста/семьи исполнитель повторно проверяет независимо от UI.

| Выбранная строка | Снимок до изменений | Остановка | Native module scan | Регистрация/запуск |
|---|---|---|---|---|
| Keep | Нет MSI/файлов этой строки | Нет | Нет из-за этой строки | Нет изменения состояния |
| Platform64 Apply/Remove | Только семейство Platform64; x64 root без вложенного OpenGlass | Platform64 | x64 | Apply: выбранная task/start; Remove: verified task cleanup; native shell removal before MSI / registration only on native Platform Apply |
| Platform32 Apply/Remove | Только семейство Platform32; x86 root | Platform32 | x86 | Аналогично; без изменения другой архитектуры |
| Settings64 Apply/Remove | Только Settings64 MSI и лист `GlassSettings` под x64 root | Platform64, т.к. меняется Native helper той же архитектуры; закрытие Settings | x64 | Shared shortcut по существующей политике, без автоматического Apply Platform; прежний работающий Platform64 восстанавливается |
| Settings32 Apply/Remove | Только Settings32 MSI и лист `GlassSettings` под x86 root | Platform32; закрытие Settings | x86 | Аналогично, независимо от Settings64 |
| OpenGlass MSI Apply/Remove | Только семейство OpenGlass, его каталог и путь | Только OpenGlass | Нет: этот Native scanner проверяет модули Platform, а не OpenGlass | Apply: собственный backend/start policy; Remove: tasks/service commit-last |
| Только Remove остатков OpenGlass, оба MSI отсутствуют | Защищённый workspace, service configuration и task XML/security; **без file/MSI snapshot Platform** | Только OpenGlass, если работает | Нет; не нужны Native helpers | Только проверенная auxiliary cleanup; без cache/shell/Platform launch и искусственной 5s launch-паузы |
| Оригинальный SUT | Собственный vendor protocol | Без остановки Glass из-за одной этой строки | Нет из-за этой строки | Отдельная последняя vendor transaction; существующие safety gates сохраняются |

## Сочетания и отказ

- Для любого сочетания Apply/Remove берётся объединение затронутых строк: например,
  Settings32 + OpenGlass Remove = Settings32 MSI/лист + OpenGlass snapshot,
  остановка Platform32/OpenGlass, сканирование x86; Platform64 не останавливается.
- Если Platform и Settings одной архитектуры выбраны одновременно, общий root
  сохраняется один раз, а MSI-снимки — по двум выбранным семействам. Полный Platform
  root snapshot пока может включать сохранённые дочерние файлы Settings; это
  консервативный снимок каталога, не дополнительное разрешение на MSI-мутацию.
- Mixed plan с residual OpenGlass не подделывает MSI baseline: реальный MSI отсутствует,
  его строка выполняет только вспомогательную очистку; другие выбранные MSI имеют свои снимки.
- Сохранение файлов/путей, переустановка MSI и остановка при rollback ограничены той же
  картой. Kept hosts не захватываются для последующего случайного restart.
  Aux-only failure восстанавливает только обратимые task/shortcut изменения и прежнее
  состояние остановленного OpenGlass; не запускает общий MSI/file rollback.
- Служба, отсутствующая на final commit (`1060`), уже удовлетворяет Remove. Это не повод
  откатывать план. Новая/изменённая/работающая/нечитаемая служба всё ещё блокирует Delete.
- Force-clean использует выбранные строки; комбинация force-clean + managed Remove
  по-прежнему запрещена. SUT mutation + managed Remove по-прежнему запрещены: общего
  атомарного vendor/service/MSI протокола нет. Невозможные OS/intent сочетания отвергает
  validated execution plan до Stop.
- Platform Remove всё ещё требует совместимый helper нужной архитектуры для shell
  unregister; если его нет, пользователь явно выбирает Settings Apply. Автоматического
  добавления Settings MSI нет. Для x64-only scan x86 monitor не запускается; при явном
  Force Unload x86 controller загружается лениво через прежние trust/protocol gates.
- Shared maintenance-cache/ARP housekeeping остаётся при реальных MSI изменениях.
  Закрытие/ожидание Settings ограничены выбранными путями и архитектурами;
  portable/чужие одноимённые процессы не закрываются. Явная общая profile cleanup
  в Unins затрагивает обе архитектуры по отдельному выбору пользователя.

## Проверки и ограничения

`procedure_plan_mock.py`: 486 комбинаций Keep/Apply/Remove на двух OS;
`scoped_snapshot_mock.py`: actual CreateSnapshot для 32 union masks;
`installer_order_mock.py`: actual StopRunningHosts, в том числе OG-only при недоступном
платформенном mutex; `engine_unlock_mock.py`: раздельные x86/x64 scan requests;
`removal_service_mock.py`: actual DeleteServiceLast, отсутствие/смена/работа/ошибки;
MSI family/context/baseline и rollback tests сохраняют отрицательные проверки.

Это portable/API-seam/source tests, **не** Windows acceptance. Нужны MSVC/WiX и VM:
каждый одиночный Apply/Remove, cross-architecture mixed plans, service исчезает до Commit,
MSI/copy/COM failure и cancellation, состояние kept hosts до/после, сохранность файлов
и настроек, корректность ARP/Current. Неизвестный custom residual root, durable recovery,
exact shell rollback, vendor atomicity и shared shortcut retarget остаются открытыми.
