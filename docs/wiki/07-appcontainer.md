# 07 · UICompat: AppContainer, PLM и `TrayXamlPolicy`

Системные хосты Windows (`StartMenuExperienceHost`, `SearchHost`, `ShellExperienceHost`,
`ShellHost`) работают под строгой изоляцией `AppContainer` и приостанавливаются
менеджером жизненного цикла (**PLM Suspend / Thaw**). `UICompat` использует SEH-блоки,
фильтрацию по идентификатору потока (`tid`) и жёсткий лимит `ModuleThread::JoinAll(8000)`.

```mermaid
flowchart LR
  subgraph OUT["HOST · вне песочницы"]
    GATE["GlassCompat Gate<br/>IsCoreWindow == true"]
    CORE["GlassCore + GDI-хуки<br/>(только Win32 / Explorer)"]
    POLICY["TrayXamlPolicy.v4<br/>108 B · Named Section (SHM)<br/>писатель: TaskbarCompat (explorer)"]
  end

  subgraph IN["APPCONTAINER SANDBOX · без GlassCore и GDI"]
    UI["UICompat.dll<br/>SEH __try/__except · tid filter"]
    IXDE["tid-scoped IXDE<br/>WinUI / XAML Tree"]
    WND["WndCompat.dll<br/>safe HWND / DWM"]
    PLM["PLM Suspend / Thaw<br/>JoinAll(8000)"]
    H1["ShellExperienceHost<br/>Action Center · Flyouts"]
    H2["ShellHost"]
    H3["StartMenuExperienceHost"]
    H4["SearchHost / SearchApp"]
  end

  GATE -->|"Skip GlassCore · Load UI + Wnd"| UI
  GATE --> CORE
  POLICY -->|"read-only"| UI
  UI --> IXDE
  UI --> WND
  UI --> PLM
  H1 --> UI
  H2 --> UI
  H3 --> UI
  H4 --> UI

  classDef sandbox fill:#3fb9501a,stroke:#3fb950;
  class UI,IXDE,WND,PLM sandbox;
```

**All-or-Nothing.** `TrayXamlPolicy.v4` пишется снаружи (из `explorer.exe`) и читается
внутри песочницы read-only, а любой C++/COM сбой перехватывается SEH — необработанное
исключение в `AppContainer` означает падение системного хоста.

**Зачем политика.** `TrayXamlPolicy.v4` (108 B, секция с DACL `WD+AC`) даёт точное
совпадение границ и радиуса углов внутри `AppContainer` с тем, что рисует
`TaskbarCompat` снаружи: XAML-поверхности не могут прочитать `HKCU` напрямую, поэтому
цвет, прозрачность и радиус передаются через разделяемую память.

**PLM.** При сворачивании всплывающего центра хост замораживается; `UICompat`
пережидает `Suspend` и повторно применяет кисти при `Thaw`, не сбрасывая системные
значения панели задач.

Источник: [docs.html · раздел 07](https://glass-platform.ru/docs.html) · ← [06](06-win11-xaml) · → [08](08-ipc)
