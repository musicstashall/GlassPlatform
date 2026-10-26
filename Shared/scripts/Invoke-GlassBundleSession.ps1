# Управление двухфазной сессией номера комплекта GlassPlatform (x64 → x86/GlassSetup).
# Вызывается из Shared/GlassBundleBuild.targets; не запускать вручную без необходимости.
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('BeginOrResumeX64', 'ContinueX86', 'Finalize', 'Cancel')]
    [string] $Action,

    [Parameter(Mandatory = $true)]
    [string] $SolutionRoot,

    [Parameter(Mandatory = $true)]
    [string] $Configuration,

    [Parameter(Mandatory = $true)]
    [string] $GlassProductVersionBase,

    [int] $GlassBundleBuildNumberOverride = 0,

    [int] $GlassBundleBuildNumberSeed = 42,

    [string] $SourceCommit = ''
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
    $raw = [System.IO.File]::ReadAllText($Path)
    if ([string]::IsNullOrWhiteSpace($raw)) { return $null }
    return $raw | ConvertFrom-Json
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
    $dir = Split-Path -Parent $LockPath
    if (-not (Test-Path -LiteralPath $dir)) {
        New-Item -ItemType Directory -Path $dir -Force | Out-Null
    }
    $fs = $null
    try {
        $fs = [System.IO.File]::Open($LockPath, [System.IO.FileMode]::OpenOrCreate, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
        & $Body
    }
    finally {
        if ($null -ne $fs) { $fs.Dispose() }
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

function Write-SessionArtifacts([string] $BuildDir, [object] $Session) {
    $sessionJson = Join-Path $BuildDir 'GlassBundleSession.json'
    $sessionProps = Join-Path $BuildDir 'GlassBundleSession.props'
    Write-JsonAtomic $sessionJson $Session
    $commaParts = Parse-Base3 $Session.GlassProductVersionBase
    $comma = "$($commaParts[0]),$($commaParts[1]),$($commaParts[2]),$($Session.GlassBundleBuildNumber)"
    Write-PropsFile $sessionProps @{
        GlassProductVersionBase   = $Session.GlassProductVersionBase
        GlassBundleBuildNumber    = $Session.GlassBundleBuildNumber
        GlassBundleVersion        = $Session.GlassBundleVersion
        GlassBundleVersionComma   = $comma
        GlassBundlePhase          = $Session.Phase
        GlassBundleSessionActive  = 'true'
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

function Test-SessionCompatible([object] $Session, [string] $Base3, [string] $Config) {
    if ($null -eq $Session) { return $false }
    if ($Session.Phase -eq 'completed') { return $false }
    if ($Session.GlassProductVersionBase -ne $Base3) {
        throw "Активная сессия комплекта для базовой версии '$($Session.GlassProductVersionBase)' не совместима с '$Base3'. Сбросьте сессию (GlassBundleCancelSession)."
    }
    if ($Session.Configuration -ne $Config) {
        throw "Активная сессия комплекта для конфигурации '$($Session.Configuration)' не совместима с '$Config'."
    }
    return $true
}

function Allocate-BuildNumber([string] $CounterPath, [int] $Override, [int] $Seed) {
    if ($Override -gt 0) {
        Test-BuildNumberRange $Override
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
            if (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration) {
                if ($existing.Phase -eq 'x86-started') {
                    throw "Активная сессия уже на фазе x86. Завершите x86/GlassSetup или выполните Cancel."
                }
                $existing.Phase = 'x64-started'
                Write-SessionArtifacts $buildDir $existing
                Write-Host "GlassBundle: продолжение сессии x64, номер $($existing.GlassBundleBuildNumber) ($($existing.GlassBundleVersion))."
                return
            }

            $build = Allocate-BuildNumber $counterPath $GlassBundleBuildNumberOverride $GlassBundleBuildNumberSeed
            $session = New-SessionObject $GlassProductVersionBase $build 'x64-started' $Configuration $SourceCommit
            Write-SessionArtifacts $buildDir $session
            Write-Host "GlassBundle: новая сессия x64, номер $build ($($session.GlassBundleVersion))."
        }

        'ContinueX86' {
            $existing = Read-JsonFile $sessionJson
            if (-not (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration)) {
                throw "Нет активной сессии x64 для комплекта. Сначала соберите Release|x64 всего solution."
            }
            if ($existing.Phase -eq 'completed') {
                throw "Сессия комплекта уже завершена. Для нового комплекта начните Release|x64."
            }
            $existing.Phase = 'x86-started'
            Write-SessionArtifacts $buildDir $existing
            Write-Host "GlassBundle: фаза x86, номер $($existing.GlassBundleBuildNumber) ($($existing.GlassBundleVersion))."
        }

        'Finalize' {
            $existing = Read-JsonFile $sessionJson
            if (-not (Test-SessionCompatible $existing $GlassProductVersionBase $Configuration)) {
                throw "Нельзя финализировать комплект: нет активной сессии."
            }
            if ($existing.Phase -ne 'x86-started') {
                throw "Финализация допустима только после фазы x86 (текущая фаза: $($existing.Phase))."
            }
            $existing.Phase = 'completed'
            $existing.CompletedUtc = ([DateTime]::UtcNow).ToString('o')
            if (-not (Test-Path -LiteralPath $bundlesDir)) {
                New-Item -ItemType Directory -Path $bundlesDir -Force | Out-Null
            }
            $manifestPath = Join-Path $bundlesDir "$($existing.GlassBundleVersion).json"
            Write-JsonAtomic $manifestPath $existing
            Write-JsonAtomic $sessionJson $existing
            $finalizeCommaParts = Parse-Base3 $existing.GlassProductVersionBase
            $finalizeComma = "$($finalizeCommaParts[0]),$($finalizeCommaParts[1]),$($finalizeCommaParts[2]),$($existing.GlassBundleBuildNumber)"
            Write-PropsFile (Join-Path $buildDir 'GlassBundleSession.props') @{
                GlassProductVersionBase   = $existing.GlassProductVersionBase
                GlassBundleBuildNumber    = $existing.GlassBundleBuildNumber
                GlassBundleVersion        = $existing.GlassBundleVersion
                GlassBundleVersionComma   = $finalizeComma
                GlassBundlePhase          = 'completed'
                GlassBundleSessionActive  = 'false'
            }
            Write-Host "GlassBundle: комплект $($existing.GlassBundleVersion) финализирован → $manifestPath"
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
