# 08 · Двухуровневый IPC, объекты ядра и бинарные секции (176 B / 108 B)

Модули работают в разных процессах и уровнях изоляции (включая `AppContainer`), поэтому
координация разделена на два контура: централизованные оконные сообщения
`GlassMessages.h` (`WM_APP + 0x201..0xD00`) и бинарные секции разделяемой памяти +
именованные объекты ядра (Event / Dead-Man Mutex / Named Pipe).

## Контур 1 · оконные сообщения

| Канал | Диапазон | Назначение |
|---|---|---|
| WndCompat | `WM_APP + 0x201..0x224` | `WM_GLASS_ATTACH_ROOT`, неблокирующие `WM_GLASS_DETACH_*`, `REAPPLY_ROOT/TASKBAR_BACKDROP`, `DWMCOLORIZATION_DISPATCH` |
| Explorer / Taskbar / UI | `WM_APP + 0x801..0xC05` | STA-маршалинг XAML-жизненного цикла: Explorer `0x801..0x80B`, Taskbar `0x901..0x906`, `GlassCompat Ensure` `0xB01..0xB06`, UICompat `0xC01..0xC05` |
| `WM_GLASS_ANIM_DETACH` | `WM_APP + 0xD00` | безопасная отвязка сабкласса анимации меню на родном UI-потоке |

## Контур 2 · объекты ядра

| Объект | Имя | Назначение |
|---|---|---|
| `StopEvent.<arch>` | `GlassPlatform.StopEvent.{x64,x86}` | глобальный сигнал остановки GUI-потоков и `MinimalUnloadWatcher` во всех инжектированных процессах |
| `HostShutdown.<arch>` | `GlassPlatform.HostShutdown.{x64,x86}` | H71/H58: хост завершил `UnhookWinEvent` + broadcast `WM_NULL` — защита от обгона OS-pin |
| `HostAlive.<arch>` | `GlassPlatform.HostAlive.{x64,x86}` | **Dead-Man Mutex** (SDDL `WD+AC`): `WAIT_ABANDONED_0` сразу сигнализирует о гибели хоста |
| `UnloadRequest.<pid>` | `Local\GlassCompat.UnloadRequest.<pid>` | точечная soft-выгрузка модулей конкретного процесса из монитора `GlassSettings` |
| `InjectMap` | `InjectMap.{x64,x86}` | `512 × 176 B` диагностическая карта (seqlock) |
| `TrayXamlPolicy` | `TrayXamlPolicy.v4` | `108 B` политика трея с DACL `WD+AC` для `AppContainer` |
| `GlassPlatform.Log` | `\.\pipe\GlassPlatform.Log` | канал логирования `GlassLogClient` |

## Разметка `InjectProcessMap::Slot` (176 B)

`Local\GlassPlatform.InjectMap.{x64,x86}` · заголовок 16 B
(`magic = 0x4D504947 'GIPM'`, `ver = 2`, `slotCount = 512`) → 90 128 B ·
`#pragma pack(push, 4)` · `sizeof(Slot) == 176`.

| Смещение | Поле | Размер | Роль |
|---|---|---|---|
| `+0` | `seq` | 4 B | `volatile LONG`, lock-free seqlock: нечётное значение = идёт запись |
| `+4` | `pid` | 4 B | идентификатор процесса |
| `+8` | `kind` | 1 B | тип инжекции |
| `+9` | `path` | 1 B | путь загрузки (обычный / CoreWindow / IXDE) |
| `+10` | `state` | 1 B | `Attached` / `Unloading` / `Dead` |
| `+11` | `reserved` | 1 B | — |
| `+12` | `moduleBits` | 4 B | `0x01` Compat · `0x02` Core · `0x04` WndCompat · `0x08` ExplorerCompat · `0x10` TaskbarCompat · `0x20` UICompat |
| `+16` | `unloadResult` | 4 B | флаги `UnloadResult` |
| `+20` | `diagFlags` | 4 B | флаги диагностики |
| `+24` | `diagLifecycle` | 4 B | состояние жизненного цикла |
| `+28` | `subclassTotal` | 4 B | число активных сабклассов |
| `+32` | `subclassKindMask` | 8 B | маска типов сабклассов |
| `+40` | `exeName[64]` | 128 B | `wchar_t[64]` имя образа |
| `+168` | `shutdownPhase` | 4 B | `ShutdownPhase 0..6` |
| `+172` | `reserved2` | 4 B | — |

Читатель (`GlassSettings`) перечитывает слот, пока чётные `seq` до и после копии не
совпадут — запись из инжектированного процесса при этом не блокируется.

Источник: [docs.html · раздел 08](https://glass-platform.ru/docs.html) · ← [07](07-appcontainer) · → [09](09-registry)
