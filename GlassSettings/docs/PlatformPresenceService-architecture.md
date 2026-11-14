# PlatformPresenceService — архитектура

## Обзор

`PlatformPresenceService` — event-driven сервис мониторинга состояния платформы (Glass + OpenGlass) в GlassSettings. Единая точка доступа к snapshot'у состояния для всех UI-страниц и контролов.

## Архитектура

```
┌─────────────────────────────────────────────────────────┐
│                Native Layer (C++)                        │
│                                                         │
│  PlatformPresenceMonitor  ──writes──▶  Shared Memory    │
│  (bg thread, 300ms-5s poll)          (PresenceState)    │
│                                                         │
│  GlassPlatformTask queries:                             │
│  - IsGlassPlatformRunning (mutex)                       │
│  - GlassAutorunTaskExists (COM Task Scheduler)          │
│  - QueryInjectLifecycleUi (composite state)             │
│  - QueryGlassAutorunUiState (composite state)           │
└─────────────────────────────────────────────────────────┘
  Хост пишет mapping для инжектированных процессов.
  Кнопки GlassSettings этот mapping не читают.
                           │
                           ▼
┌─────────────────────────────────────────────────────────┐
│              PlatformPresenceService (C#)                │
│                                                         │
│  ┌──────────────┐    ┌──────────────────────────┐       │
│  │ Watcher STA  │◀───│ PresenceChanged /        │       │
│  │ 500 мс +     │    │ NotifyLifecycleChanged   │       │
│  │ прямой опрос │    └──────────────────────────┘       │
│  └──────┬───────┘                                       │
│         ▼                                               │
│  ┌─────────────────────────────────────────────┐        │
│  │ NotifySubscribers(snap)                     │        │
│  │ → all Action<PresenceSnapshot> callbacks    │        │
│  │ → PresenceChanged event (back-compat)       │        │
│  └─────────────────────────────────────────────┘        │
└──────────────────────────┬──────────────────────────────┘
                           │ UI thread dispatch
                           ▼
┌─────────────────────────────────────────────────────────┐
│                   Consumers (UI)                        │
│                                                         │
│  PlatformSchedulerRow  — TaskExists, Refresh()          │
│  HomePage              — OpenGlassLine, GlassPlatformLine│
│  OpenGlassBorderPolicyCoordinator                       │
│  OpenGlassRootBackdropNoticeCoordinator                 │
│  OpenGlassUiPolicy                                      │
│  SwcaDwmBackdropNoticeCoordinator                       │
└─────────────────────────────────────────────────────────┘
```

## PresenceSnapshot — поля

| Поле | Тип | Описание |
|------|-----|----------|
| `Revision` | uint | Версия снимка (монотонно растёт) |
| `GlassState` | int | PresenceUiState для native arch |
| `OpenGlassOrchestration` | int | PresenceUiState для OG |
| `OpenGlassPipelinePhase` | int | PipelinePhase (Idle/Starting/Active/Stopping) |
| `RootBackdropMode` | uint | 0=no backdrop, 1=pipeline, 2=default |
| `OpenGlassPipelineActive` | uint | 1 = backdrop pipeline активен |
| `OpenGlassHostProcessRunning` | uint | 1 = OG host process жив |
| `OpenGlassBackend` | int | OpenGlassBackend enum |
| `GlassAutorunState` | int | native arch (back-compat) |
| `InjectLifecycleUi` | int | native arch (back-compat) |
| `StopEventSignaled` | uint | 1 = stop event signaled для native arch |
| **Dual-arch Glass** | | |
| `GlassTaskExistsX64` | uint | 1 = autorun task существует (x64) |
| `GlassTaskExistsX86` | uint | 1 = autorun task существует (x86) |
| `InjectLifecycleUiX64` | int | InjectLifecycleUi для x64 |
| `InjectLifecycleUiX86` | int | InjectLifecycleUi для x86 |
| `GlassAutorunStateX64` | int | AutorunUiState для x64 |
| `GlassAutorunStateX86` | int | AutorunUiState для x86 |
| **OG x64 only** | | |
| `OgLegacyTaskExists` | uint | 1 = оркестрация OG установлена (служба или задача) |
| `OgCurrentTaskExists` | uint | 1 = `OpenGlassOrchestration` не `NoTask` |

## Event-driven модель

### Subscribe / Unsubscribe

```csharp
// Подписка — получает текущий snap немедленно + уведомления при изменениях
PlatformPresenceService.Instance.Subscribe(snap =>
{
    // snap — актуальное состояние платформы
    // Вызывается на UI thread
});

// Отписка
PlatformPresenceService.Instance.Unsubscribe(callback);
```

### PresenceChanged (back-compat)

```csharp
PlatformPresenceService.Instance.PresenceChanged += OnPresenceChanged;
```

Существующие потребители продолжают работать через `PresenceChanged`. Новый паттерн `Subscribe/Unsubscribe` доступен для будущих потребителей.

## Watcher

Снимок кнопок Glass и OpenGlass собирает поток `PlatformPresence.Watcher`, пока открыт GlassSettings. Общая память хоста не источник этих полей: процесс DWM не видит dual-arch Glass.

1. **STA thread** — COM-запросы (Task Scheduler, SCM) остаются на этом потоке
2. **500 мс** или немедленное пробуждение (`PresenceChanged`, `NotifyLifecycleChanged`)
3. **Прямой опрос** — `BuildFallbackSnapshot()` (mutex, планировщик, SCM). Обход всего дерева задач не выполняется на UI-потоке
4. **Change detection** — `SnapshotEquals()`; уведомление только при реальном изменении
5. **NotifySubscribers** — колбэки и `PresenceChanged` на UI через `DispatcherQueue.TryEnqueue`

## Жизненный цикл

```
Attach(dispatcher)
    → RefreshSnapshot(forceNotify: true)
    → SyncFallbackPoll() — запуск fallback если нужно

StartPresenceWatchAfterShellReady()
    → EnsurePresenceWait() — named wait thread
    → RefreshSnapshot(forceNotify: true, raiseEvent: true)

Subscribe(callback)     ← UI контрольы
Unsubscribe(callback)   ← UI контрольы при Unloaded

NotifyLifecycleChanged() ← после Start/Stop Glass
    → InvalidateOGCache() — срок кэша сброшен, последнее состояние OG сохранено
    → SignalWatcherRefresh()
    → RaisePresenceTimed() — UI сразу, даже если снимок не изменился (снять busy)

Dispose()
    → Cancel all threads
    → Join with timeout
```

## Потребители

| Потребитель | Что читает | Способ |
|-------------|-----------|--------|
| `PlatformSchedulerRow.TaskExists` | `GlassTaskExistsX64/X86`, `OgLegacyTaskExists` | snapshot |
| `PlatformSchedulerRow.Refresh()` | `InjectLifecycleUiX64/X86`, `GlassAutorunStateX64/X86`, `OpenGlassOrchestration` | snapshot |
| `HomeStatusSummary.QueryGlassPlatformUi(arch)` | `InjectLifecycleUiX64/X86`, `GlassAutorunStateX64/X86` | snapshot |
| `HomeStatusSummary.GetOpenGlassState()` | `OpenGlassOrchestration` | snapshot |
| `OpenGlassBorderPolicyCoordinator` | `OpenGlassPipelineActive` | snapshot |
| `RootBackdropModePolicy` | `RootBackdropMode` | snapshot |

## Архитектура данных

```
Glass x64 ─┐
Glass x86 ─┼─▶ BuildFallbackSnapshot() → _snapshot
OG        ─┘         ▲
                     │ будит watcher
        PresenceChanged / NotifyLifecycleChanged

Хост по-прежнему пишет свой mapping (инжектированные процессы).
GlassSettings для кнопок читает только _snapshot.
```

## Anti-patterns

- ❌ Не вызывать `GlassPlatformService.GlassAutorunTaskExists()` на UI thread — использовать snapshot
- ❌ Не читать `OpenGlassHostService.QueryOpenGlassHostUiState()` на UI thread — использовать snapshot
- ❌ Не обходить дерево задач планировщика на UI-потоке и не обнулять последнее состояние OG при `InvalidateOGCache` — иначе кнопка OpenGlass на старте Glass становится серой
- ❌ Не ждать schtasks, mutex и baseline старта Glass на UI-потоке
- ❌ Не подписываться дважды без отписки — `AddSubscriber()` с reference counting
- ✅ Использовать `Subscribe(cb)` для новых потребителей
- ✅ Использовать `PresenceChanged` для обратной совместимости
