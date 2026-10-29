# Управление двухфазной сессией номера комплекта GlassPlatform (x64 → x86/GlassSetup).
# Вызывается из Shared/GlassBundleBuild.targets; не запускать вручную без необходимости.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('BeginOrResumeX64', 'ContinueX86', 'Finalize', 'FinalizeIfX86Ready', 'Cancel')]
    [string] $Action,

    [Parameter(Mandatory = $true)]
    [string] $SolutionRoot,

    [Parameter(Mandatory = $true)]
    [string] $Configuration,

    [Parameter(Mandatory = $true)]
    [string] $GlassProductVersionBase,

    [int] $GlassBundleBuildNumberOverride = 0,

    [int] $GlassBundleBuildNumberSeed = 42,

    [string] $SourceCommit = '',

    # Только x64 native DLL: после финализации начать новый комплект. Остальные проекты — повторная сборка без нового номера.
    [switch] $AllowNewBundleAfterCompleted
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Normalize-Root([string] $Root) {
    if (-not $Root.EndsWith('\')) { $Root += '\' }
    return $Root
}

function Ensure-BuildDir([string] $Root) {
    $dir = Join-Path $Root 'Build'
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    return $dir
}

function Read-JsonFile([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path)) { return $null }
    $attempts = 0
    while ($true) {
        $attempts++
        try {
            $raw = [System.IO.File]::ReadAllText($Path)
            if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
            return $raw | ConvertFrom-Json
        }
        catch {
            if ($attempts -ge 5) { throw }
            Start-Sleep -Milliseconds 80
        }
    }
}

function Write-JsonAtomic([string] $Path, [object] $Object) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $tmp = "$Path.tmp"
    $json = $Object | ConvertTo-Json -Depth 6 -Compress
    [System.IO.File]::WriteAllText($tmp, $json, [System.Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function Write-PropsFile([string] $Path, [hashtable] $Props) {
    $dir = Split-Path -Parent $Path
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add('<Project>')
    $lines.Add('  <PropertyGroup>')
    foreach ($kv in $Props.GetEnumerator() | Sort-Object Name) {
        $val = [System.Security.SecurityElement]::Escape([string]$kv.Value)
        $lines.Add("    <$($kv.Name)>$val</$($kv.Name)>")
    }
    $lines.Add('  </PropertyGroup>')
    $lines.Add('</Project>')
    $tmp = "$Path.tmp"
    [System.IO.File]::WriteAllLines($tmp, $lines, [System.Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $Path) { Remove-Item -LiteralPath $Path -Force }
    Move-Item -LiteralPath $tmp -Destination $Path -Force
}

function Format-BundleVersion([string] $Base3, [int] $Build) {
    return "$Base3.$($Build.ToString('0000'))"
}

function Parse-Base3([string] $Base3) {
    if ($Base3 -notmatch '^\d+\.\d+\.\d+$') {
        throw "GlassProductVersionBase имеет неверный формат: '$Base3' (ожидается Major.Minor.Patch)."
    }
    $parts = $Base3.Split('.')
    return [int[]]@([int]$parts[0], [int]$parts[1], [int]$parts[2])
}

function Test-BuildNumberRange([int] $Build) {
    if ($Build -le 0) { throw "Номер комплекта должен быть положительным целым, получено: $Build." }
    if ($Build -gt 65535) { throw "Номер комплекта $Build превышает предел FileVersion (65535)." }
}

function Invoke-WithSessionLock([string] $LockPath, [scriptblock] $Body) {
    # FileShare.None на File.Open не блокирует — при /m>1 MSBuild получает IOException. Mutex ждёт.
    $dir = Split-Path -Parent $LockPath
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $mutexName = 'Local\GlassProject_GlassBundleSession'
    $mutex = New-Object System.Threading.Mutex($false, $mutexName)
    $acquired = $false
    try {
        $acquired = $mutex.WaitOne(180000)
        if (-not $acquired) {
            throw 'GlassBundle: не удалось захватить lock сессии за 180 с (параллельная сборка solution).'
        }
        & $Body
    }
    finally {
        if ($acquired) {
            try { [void]$mutex.ReleaseMutex() } catch { }
        }
        $mutex.Dispose()
    }
}

function New-SessionObject([string] $Base3, [int] $Build, [string] $Phase, [string] $Config, [string] $Commit) {
    $bundleVersion = Format-BundleVersion $Base3 $Build
    return [ordered]@{
        GlassProductVersionBase = $Base3
        GlassBundleBuildNumber  = $Build
        GlassBundleVersion      = $bundleVersion
        Phase                   = $Phase
        Configuration           = $Config
        StartedUtc              = ([DateTime]::UtcNow).ToString('o')
        SourceCommit            = $Commit
    }
}

function Test-SessionSameProduct([object] $Session, [string] $Base3, [string] $Config) {
    if ($null -eq $Session) { return $false }
    if ($Session.GlassProductVersionBase -ne $Base3) {
        throw "Активная сессия комплекта для базовой версии '$($Session.GlassProductVersionBase)' не совместима с '$Base3'. Сбросьте сессию (GlassBundleCancelSession)."
    }
    if ($Session.Configuration -ne $Config) {
        throw "Активная сессия комплекта для конфигурации '$($Session.Configuration)' не совместима с '$Config'."
    }
    return $true
}

function Write-SessionArtifacts([string] $BuildDir, [object] $Session) {
    $sessionJson = Join-Path $BuildDir 'GlassBundleSession.json'
    $sessionProps = Join-Path $BuildDir 'GlassBundleSession.props'
    $record = if ($Session -is [System.Collections.IDictionary]) { $Session } else { ConvertTo-SessionRecord $Session @{} }
    Write-JsonAtomic $sessionJson $record
    $commaParts = Parse-Base3 $record.GlassProductVersionBase
    $comma = "$($commaParts[0]),$($commaParts[1]),$($commaParts[2]),$($record.GlassBundleBuildNumber)"
    $sessionActive = if ($record.Phase -eq 'completed') { 'false' } else { 'true' }
    Write-PropsFile $sessionProps @{
        GlassProductVersionBase   = $record.GlassProductVersionBase
        GlassBundleBuildNumber    = $record.GlassBundleBuildNumber
        GlassBundleVersion        = $record.GlassBundleVersion
        GlassBundleVersionComma   = $comma
        GlassBundlePhase          = $record.Phase
        GlassBundleSessionActive  = $sessionActive
    }
}

function Read-Counter([string] $CounterPath) {
    if (-not (Test-Path -LiteralPath $CounterPath)) { return 0 }
    $text = ([System.IO.File]::ReadAllText($CounterPath)).Trim()
    if ($text -match '^\d+$') { return [int]$text }
    return 0
}

function Write-Counter([string] $CounterPath, [int] $Value) {
    $dir = Split-Path -Parent $CounterPath
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $tmp = "$CounterPath.tmp"
    [System.IO.File]::WriteAllText($tmp, [string]$Value, [System.Text.UTF8Encoding]::new($false))
    if (Test-Path -LiteralPath $CounterPath) { Remove-Item -LiteralPath $CounterPath -Force }
    Move-Item -LiteralPath $tmp -Destination $CounterPath -Force
}

function ConvertTo-SessionRecord([object] $Session, [hashtable] $Overrides) {
    $record = [ordered]@{
        GlassProductVersionBase = [string]$Session.GlassProductVersionBase
        GlassBundleBuildNumber  = [int]$Session.GlassBundleBuildNumber
        GlassBundleVersion      = [string]$Session.GlassBundleVersion
        Phase                   = [string]$Session.Phase
        Configuration           = [string]$Session.Configuration
        StartedUtc              = [string]$Session.StartedUtc
        SourceCommit            = [string]$Session.SourceCommit
    }
    if ($null -ne $Session.PSObject.Properties['CompletedUtc']) {
        $record.CompletedUtc = [string]$Session.CompletedUtc
    }
    foreach ($kv in $Overrides.GetEnumerator()) {
        $record[$kv.Key] = $kv.Value
    }
    return $record
}

function Test-SessionCompatible([object] $Session, [string] $Base3, [string] $Config) {
    if ($null -eq $Session) { return $false }
    if ($Session.Phase -eq 'completed') { return $false }
    if (-not (Test-SessionSameProduct $Session $Base3 $Config)) { return $false }
    return $true
}

function Complete-Session([object] $Session) {
    $completedUtc = ([DateTime]::UtcNow).ToString('o')
    $record = ConvertTo-SessionRecord $Session @{
        Phase        = 'completed'
        CompletedUtc = $completedUtc
    }
    if (-not (Test-Path -LiteralPath $bundlesDir)) {
        New-Item -ItemType Directory -Path $bundlesDir -Force | Out-Null
    }
    $manifestPath = Join-Path $bundlesDir "$($record.GlassBundleVersion).json"
    Write-JsonAtomic $manifestPath $record
    Write-JsonAtomic $sessionJson $record
    $finalizeCommaParts = Parse-Base3 $record.GlassProductVersionBase
    $finalizeComma = "$($finalizeCommaParts[0]),$($finalizeCommaParts[1]),$($finalizeCommaParts[2]),$($record.GlassBundleBuildNumber)"
    Write-PropsFile (Join-Path $buildDir 'GlassBundleSession.props') @{
        GlassProductVersionBase   = $record.GlassProductVersionBase
        GlassBundleBuildNumber    = $record.GlassBundleBuildNumber
        GlassBundleVersion        = $record.GlassBundleVersion
        GlassBundleVersionComma   = $finalizeComma
        GlassBundlePhase          = 'completed'
        GlassBundleSessionActive  = 'false'
    }
    Write-Host "GlassBundle: комплект $($record.GlassBundleVersion) финализирован → $manifestPath"
}

function Allocate-BuildNumber([string] $CounterPath, [int] $Override, [int] $Seed) {
    if ($Override -gt 0) {
        Test-BuildNumberRange $Override
        $last = Read-Counter $CounterPath
        if ($Override -lt $last) {
            throw "GlassBundleBuildNumber=$Override меньше локального счётчика $last. Не используйте Override для повторного номера — продолжите сессию."
        }
        Write-Counter $CounterPath $Override
        return $Override
    }
    $last = Read-Counter $CounterPath
    $next = if ($last -le 0) { $Seed } else { $last + 1 }
    Test-BuildNumberRange $next
    Write-Counter $CounterPath $next
    return $next
}

$root = Normalize-Root $SolutionRoot
$buildDir = Ensure-BuildDir $root
$lockPath = Join-Path $buildDir 'GlassBundleSession.lock'
$sessionJson = Join-Path $buildDir 'GlassBundleSession.json'
$counterPath = Join-Path $buildDir 'GlassBundleBuildNumber.txt'
$bundlesDir = Join-Path $buildDir 'GlassBundles'

if ($Configuration -ne 'Release') {
    throw "Сессия комплекта поддерживается только для Configuration=Release (получено '$Configuration')."
}

Invoke-WithSessionLock $lockPath {
    switch ($Action) {
        'BeginOrResumeX64' {
            $existing = Read-JsonFile $sessionJson
            if ($null -ne $existing -and (Test-SessionSameProduct $existing $GlassProductVersionBase $Configuration)) {
                if ($existing.Phase -eq 'completed') {
                    if (-not $AllowNewBundleAfterCompleted) {
                        Write-SessionArtifacts $buildDir $existing
                        Write-Host "GlassBundle: повторная сборка, комплект $($existing.GlassBundleVersion) (номер без изменений, Phase=completed)."
                        return
                    }
                }
                elseif (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration) {
                    if ($existing.Phase -eq 'x86-started') {
                        Write-SessionArtifacts $buildDir $existing
                        Write-Host "GlassBundle: x64 при фазе x86, номер $($existing.GlassBundleBuildNumber) ($($existing.GlassBundleVersion)), фаза без изменений."
                        return
                    }
                    $record = ConvertTo-SessionRecord $existing @{ Phase = 'x64-started' }
                    Write-SessionArtifacts $buildDir $record
                    Write-Host "GlassBundle: продолжение сессии x64, номер $($record.GlassBundleBuildNumber) ($($record.GlassBundleVersion))."
                    return
                }
            }

            $build = Allocate-BuildNumber $counterPath $GlassBundleBuildNumberOverride $GlassBundleBuildNumberSeed
            $session = New-SessionObject $GlassProductVersionBase $build 'x64-started' $Configuration $SourceCommit
            Write-SessionArtifacts $buildDir $session
            Write-Host "GlassBundle: новая сессия x64, номер $build ($($session.GlassBundleVersion))."
        }

        'ContinueX86' {
            $existing = Read-JsonFile $sessionJson
            if ($null -eq $existing) {
                Write-Host 'GlassBundle: ContinueX86 пропущен (сессия ещё не создана — нормально для Win32-утилит в параллельном x64-проходе).'
                return
            }
            if ($existing.Phase -eq 'completed' -and (Test-SessionSameProduct $existing $GlassProductVersionBase $Configuration)) {
                Write-SessionArtifacts $buildDir $existing
                Write-Host "GlassBundle: повторная x86-сборка, комплект $($existing.GlassBundleVersion) (номер без изменений, Phase=completed)."
                return
            }
            if (-not (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration)) {
                throw "Нет активной сессии x64 для комплекта. Сначала соберите Release|x64 всего solution."
            }
            if ($existing.Phase -eq 'x86-started') {
                Write-Host "GlassBundle: фаза x86 уже активна, номер $($existing.GlassBundleBuildNumber) ($($existing.GlassBundleVersion))."
                return
            }
            $record = ConvertTo-SessionRecord $existing @{ Phase = 'x86-started' }
            Write-SessionArtifacts $buildDir $record
            Write-Host "GlassBundle: фаза x86, номер $($record.GlassBundleBuildNumber) ($($record.GlassBundleVersion))."
        }

        'Finalize' {
            $existing = Read-JsonFile $sessionJson
            if (-not (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration)) {
                throw "Нельзя финализировать комплект: нет активной сессии."
            }
            if ($existing.Phase -ne 'x86-started') {
                throw "Финализация допустима только после фазы x86 (текущая фаза: $($existing.Phase))."
            }
            Complete-Session $existing
        }

        'FinalizeIfX86Ready' {
            $existing = Read-JsonFile $sessionJson
            if ($null -eq $existing) {
                Write-Host 'GlassBundle: финализация пропущена (нет сессии).'
                return
            }
            if ($existing.Phase -eq 'completed') {
                Write-Host "GlassBundle: комплект $($existing.GlassBundleVersion) уже финализирован."
                return
            }
            if (-not (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration)) {
                Write-Host 'GlassBundle: финализация пропущена (сессия несовместима).'
                return
            }
            if ($existing.Phase -ne 'x86-started') {
                Write-Host "GlassBundle: финализация пропущена (фаза $($existing.Phase), нужна x86-started)."
                return
            }
            Complete-Session $existing
        }

        'Cancel' {
            $existing = Read-JsonFile $sessionJson
            if ($null -eq $existing -or $existing.Phase -eq 'completed') {
                if (Test-Path -LiteralPath $sessionJson) { Remove-Item -LiteralPath $sessionJson -Force }
                if (Test-Path -LiteralPath (Join-Path $buildDir 'GlassBundleSession.props')) {
                    Remove-Item -LiteralPath (Join-Path $buildDir 'GlassBundleSession.props') -Force
                }
                Write-Host 'GlassBundle: активная сессия отсутствует (cancel — no-op).'
                return
            }
            $num = $existing.GlassBundleBuildNumber
            if (Test-Path -LiteralPath $sessionJson) { Remove-Item -LiteralPath $sessionJson -Force }
            if (Test-Path -LiteralPath (Join-Path $buildDir 'GlassBundleSession.props')) {
                Remove-Item -LiteralPath (Join-Path $buildDir 'GlassBundleSession.props') -Force
            }
            Write-Host "GlassBundle: сессия отменена; номер $num не будет повторно использован."
        }
    }
}
