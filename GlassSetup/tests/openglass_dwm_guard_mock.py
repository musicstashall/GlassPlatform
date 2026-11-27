"""Source gates for the OpenGlass DWM crash guard in GlassSetup (LaunchSelectedProducts / EnsureOpenGlassBeforePlatform)."""
from pathlib import Path
root = Path(__file__).resolve().parents[2]
engine = (root / 'GlassSetup/src/InstallEngine.cpp').read_text()
header = (root / 'GlassSetup/src/GlassProducts.h').read_text()
guard = (root / 'GlassShared/src/OpenGlassDwmGuard.cpp').read_text()
guardHeader = (root / 'GlassShared/include/OpenGlassDwmGuard.h').read_text()
policy = (root / 'GlassShared/include/OpenGlassDwmGuardPolicy.h').read_text()

# --- политика запуска: 2026-11-27, всегда при autostartAllowed && valid
assert 'return startup.autostartAllowed && startup.valid && (!startup.filesPresent || startup.wasRunning);' not in header
assert 'return startup.autostartAllowed && startup.valid;' in header
assert 'kOpenGlassAutostartMinBuild = 22631' in header

# --- установщик подключил сторожа
assert '#include <OpenGlassDwmGuard.h>' in engine
assert 'Shared::OpenGlassDwmGuard::ShouldSuppressOpenGlassStart(installedOgVersion)' in engine
assert 'Shared::OpenGlassDwmGuard::Arm(Shared::OpenGlassDwmGuard::kArmedBySetup, installedOgVersion)' in engine
assert 'Shared::OpenGlassDwmGuard::Disarm()' in engine
assert 'Shared::OpenGlassDwmGuard::IsWatching()' in engine
assert 'Shared::OpenGlassDwmGuard::IsQuarantined()' in engine
assert 'Shared::OpenGlassDwmGuard::ReadMarker(guardMarker)' in engine

# --- EnsureOpenGlassBeforePlatform: вооружение до старта, снятие при отказе
ensure = engine[engine.index('bool InstallEngine::EnsureOpenGlassBeforePlatform()'):engine.index('\nvoid InstallEngine::LaunchSelectedProducts()', engine.index('EnsureOpenGlassBeforePlatform()'))]
assert ensure.index('Arm(Shared::OpenGlassDwmGuard::kArmedBySetup, record.version)') < ensure.index('StartOpenGlassServicePreservingStartType(startType)')
assert 'IsWatching()' in ensure  # повторного Arm нет, если блоком запуска уже вооружено
assert 'Disarm()' in ensure

# --- порядок в блоке запуска OG: подавление по карантину -> проверка путей -> вооружение -> старт
og = engine[engine.index('    bool shouldStartOg=false;'):engine.index('    const bool platformLaunch=', engine.index('    bool shouldStartOg=false;'))]
assert og.index('ShouldSuppressOpenGlassStart(installedOgVersion)') < og.index('ValidateOpenGlassPaths')
assert og.index('ValidateOpenGlassPaths') < og.index('Arm(Shared::OpenGlassDwmGuard::kArmedBySetup, installedOgVersion)')
assert og.index('Arm(Shared::OpenGlassDwmGuard::kArmedBySetup, installedOgVersion)') < og.index('StartOpenGlassServicePreservingStartType(prior_.ogServiceStartType)') if 'StartOpenGlassServicePreservingStartType(prior_.ogServiceStartType)' in og else True
assert 'Disarm()' in og  # отказ запуска / OG не сообщил running
assert og.index('Arm(') < og.index('AddFinishWarning(warnOg, JoinWarningDetail(labels_.warnOpenGlassHint,')

# --- ожидание сторожа после запуска платформы, до сертификатов/оболочки
settle = engine.index('waiting for the OpenGlass DWM crash guard')
assert engine.index('if (ogReady && !RestorePriorHosts(true))') < settle
assert settle < engine.index('// Removal must not recreate the native menu')
assert 'GetTickCount64() - guardWaitStart < 33000' in engine

# --- карантин в стороже: маркер -> TryShutdown -> DEMAND_START + /DISABLE
assert 'Shared::OpenGlassHostLaunch::TryShutdown();' in guard
assert 'DisableOgServiceAutostartKeepingDisabled()' in guard  # DEMAND_START; DISABLED пользователя не перетираем
assert 'SERVICE_DISABLED' in guard
assert 'RunSchtasksOnTask(L"OpenGlass Host", L"/Change /DISABLE")' in guard
assert 'RunSchtasksOnTask(L"OpenGlass Legacy Host", L"/Change /DISABLE")' in guard
assert 'kStateQuarantined' in guard and 'kValueQuarantine, 1' in guard

# --- режим Danger и стоп после второго краша
assert 'EvaluateCrash' in guard and 'kStateDanger' in guard
assert 'QuarantineDanger' in policy and 'QuarantinePair' in policy
assert 'QuarantineFuse' in policy and 'kFuseCrashCount' in policy
assert 'EnterDanger' in policy
assert 'kMaxWatchSeconds' in policy and 'kCleanExitSeconds' in policy

# --- карантин снимается строго более новой версией или вручную
assert 'IsVersionStrictlyGreater(installedOgVersion, info.ogVersion)' in guard
assert 'ClearQuarantine()' in guard

# --- маркер переживает рестарт сессии (база 30-с окна при повторном Arm)
assert 'ReadCrashTimes' in guard and 'DropExpired' in guard

# --- кросс-процессная единственность: именованный мьютекс + событие смены состояния
assert 'Local\\\\GlassPlatform.OpenGlassDwmGuard.Watch' in guard
assert 'Local\\\\GlassPlatform.OpenGlassDwmGuard.StateChanged' in guard
assert 'CreateMutexW' in guard and 'ERROR_ALREADY_EXISTS' in guard
assert 'CreateEventW' in guard and 'SetEvent' in guard
assert 'ReleaseCrossProcessObjects' in guard

# --- счётчик цикла наблюдения и жёсткий потолок 120 с / досрочный чистый выход за 30 с
assert 'g_totalCrashes' in guard
assert 'kLastResultTimeout' in (root / 'GlassShared/include/OpenGlassDwmGuard.h').read_text()
assert 'kMaxWatchSeconds) * 1000ull' in guard
assert 'kCleanExitSeconds) * 1000ull' in guard
assert 'kReasonFuseTotal' in guard

# --- тотальность идентичности dwm (PID + время создания)
assert 'identityChanged' in guard

# --- сторожа нет в пути «старт не состоялся»
eng = engine
assert eng.count('Disarm()') >= 3

print('PASS: GlassSetup DWM-guard wiring: suppress -> arm -> start -> watch -> quarantine note; Ensure barrier; marker; danger+fuse semantics; cross-process mutex/event')
