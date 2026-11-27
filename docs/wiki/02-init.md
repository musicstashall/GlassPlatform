# 02 · Вход в процесс, Gate R7 и CoreWindow-ветвление

Когда Win32-хук доставляет `GlassCompat.dll` в целевой процесс, система не инжектирует
весь графический стек слепо. Поток инициализации проходит через проверку остановки
(`R7`), фильтрацию списков процессов (64 `kAttachList` / 21 `kBlockList`) и строгую
классификацию: в `CoreWindow`-хосты **`GlassCore.dll` не загружается никогда**.

```mermaid
flowchart TD
  A["DllMain → Init"] --> B["InitWorker"]
  B --> C{"Gate R7<br/>HostAlive доступен, GlassStop снят?"}
  C -->|"Да (R7)"| STOP["Stop / Dead<br/>Abort + Free"]
  C -->|"Нет"| D{"Allow List?<br/>64 attach · 21 block · 55 built-in"}
  D -->|"Нет"| STOP
  D -->|"Да"| E{"CoreWindow?<br/>UWP / XAML"}
  E -->|"Да (UWP)"| F["UICompat + Eager WndCompat<br/>без GlassCore и GDI-хуков"]
  F --> W["WaitExit"]
  E -->|"Нет → обычное HWND"| G{"explorer 11?<br/>IXDE trigger"}
  G -->|"Да (11)"| H["ExplorerCompat + TaskbarCompat<br/>Win11 IXDE Lazy"]
  G -->|"Нет"| I["GlassCore 33 Hooks<br/>+ WndCompat 21 Controls"]
  I --> J["Paint + Subclassing"]
  H --> W

  classDef gate fill:#1f6feb1a,stroke:#1f6feb;
  classDef abort fill:#f851491a,stroke:#f85149;
  classDef core fill:#3fb9501a,stroke:#3fb950;
  class C,D,E,G gate;
  class STOP abort;
  class I,J core;
```

Фильтры по веткам («Все ветки / Обычный Win32 / Win11 explorer / CoreWindow / Stop»)
на сайте интерактивны; здесь роль ветки видна по цвету узла.

**R7-гейт перед стартом.** Нативный приёмник в `InitWorker` проверяет доступность
`HostAlive.<arch>` и отсутствие сигнала `GlassStop` до открытия любых графических
дескрипторов — это исключает гонки при закрытии хоста.

**Почему CoreWindow пропускает `GlassCore`.** В `ShellExperienceHost`,
`StartMenuExperienceHost` и `SearchHost` строгая песочница `AppContainer` и
DComp-композиция XAML несовместимы с GDI/UXTheme-хуками `GlassCore`. Туда грузятся
только `UICompat` и `WndCompat`.

Источник: [docs.html · раздел 02](https://glass-platform.ru/docs.html) · ← [01](01-overview) · → [03](03-hooks-pipeline)
