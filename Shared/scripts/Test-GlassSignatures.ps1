#Requires -Version 5.1
<#
.SYNOPSIS
    Release gate: verifies the Authenticode signatures of every first-party artifact of a complete
    Release build (x64 phase + x86 phase) before it is published.

.DESCRIPTION
    Builds the artifact list (platform DLL/EXE of both architectures, GlassSettings publish exe,
    GlassSettings.Native.dll, GlassUnins.exe, the five MSI and GlassSetup.exe) and runs
    Invoke-GlassSign.ps1 -Mode Verify -Policy Require against it. The expected thumbprint comes from
    -Thumbprint, otherwise from GlassSignThumbprint in Shared\GlassSign.targets, otherwise from the
    author key in the certificate store (subject match). Missing artifacts are errors unless
    -AllowMissing is given. The sparse MSIX of the shell menu is only reported (separate dev
    certificate, see GlassSetup\README.md).

    Provider-agnostic: works unchanged for a SignPath certificate (pass its thumbprint).
    Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts in the ANSI code page.

.EXAMPLE
    powershell -NoProfile -ExecutionPolicy Bypass -File Shared\scripts\Test-GlassSignatures.ps1
#>
[CmdletBinding()]
param(
    [string]$SolutionRoot = '',
    [string]$Thumbprint = '',
    [string]$Subject = '',
    [string]$Configuration = 'Release',
    [switch]$AllowMissing
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if (-not $SolutionRoot) { $SolutionRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).ProviderPath }
$SolutionRoot = $SolutionRoot.TrimEnd('\')
$signTargets = Join-Path $SolutionRoot 'Shared\GlassSign.targets'
$signScript = Join-Path $PSScriptRoot 'Invoke-GlassSign.ps1'

function Get-TargetsProperty([string]$Name) {
    if (-not (Test-Path -LiteralPath $signTargets -PathType Leaf)) { return '' }
    [xml]$xml = Get-Content -LiteralPath $signTargets -Raw
    $node = $xml.SelectSingleNode("//PropertyGroup/$Name")
    if ($null -eq $node) { return '' }
    $value = $node.InnerText.Trim()
    if ($value -like '*$(*') { return '' }
    return $value
}

if (-not $Thumbprint) { $Thumbprint = Get-TargetsProperty 'GlassSignThumbprint' }
if (-not $Subject) { $Subject = Get-TargetsProperty 'GlassSignSubject' }
if (-not $Subject) { $Subject = 'CN=Stanislav K.' }

$build = Join-Path $SolutionRoot 'Build'
$platformFiles = @(
    'ExplorerCompat.dll', 'GlassCompat.dll', 'GlassCore.dll', 'GlassShellExt.dll', 'GlassShellHost.exe',
    'TaskbarCompat.dll', 'UICompat.dll', 'WndCompat.dll', 'GlassSettings.Native.dll',
    'GlassSettings\GlassSettings.exe'
)

$required = New-Object 'System.Collections.Generic.List[string]'
foreach ($arch in @('x64', 'x86')) {
    foreach ($name in $platformFiles) { $required.Add((Join-Path $build "$arch\$Configuration\$name")) }
}
$required.Add((Join-Path $build "GlassUnins\$Configuration\GlassUnins.exe"))
foreach ($msi in @('GlassSettings-x64', 'GlassSettings-x86', 'Glass-x64', 'Glass-x86', 'OpenGlass-x64')) {
    $required.Add((Join-Path $build "GlassSetup\msi\$Configuration\$msi\$msi.msi"))
}
$required.Add((Join-Path $build "GlassSetup\$Configuration\GlassSetup.exe"))

# Developer tool, not part of the package: checked only when present.
$optional = New-Object 'System.Collections.Generic.List[string]'
foreach ($arch in @('x64', 'x86')) {
    $logViewer = Join-Path $build "$arch\$Configuration\LogViewer.exe"
    if (Test-Path -LiteralPath $logViewer -PathType Leaf) { $optional.Add($logViewer) }
}

Write-Host "GlassSign release gate"
Write-Host ("  solution root : {0}" -f $SolutionRoot)
Write-Host ("  subject       : {0}" -f $Subject)
if ($Thumbprint) { Write-Host ("  thumbprint pin: {0}" -f $Thumbprint) } else { Write-Host '  thumbprint pin: (none - GlassSignThumbprint is empty; the author key in the store is used)' }
Write-Host ("  artifacts     : {0} required, {1} optional" -f $required.Count, $optional.Count)
Write-Host ''

$all = @($required) + @($optional)
# Windows PowerShell drops empty-string arguments to native commands: add -Thumbprint only when set.
$arguments = @(
    '-NoProfile', '-NonInteractive', '-ExecutionPolicy', 'Bypass', '-File', $signScript,
    '-Mode', 'Verify', '-Policy', 'Require',
    '-Paths', ($all -join ';'),
    '-Subject', $Subject,
    '-Context', 'release gate'
)
if ($Thumbprint) { $arguments += @('-Thumbprint', $Thumbprint) }
if (-not $AllowMissing) { $arguments += '-RequireFiles' }

& powershell.exe @arguments
$verifyExit = $LASTEXITCODE

Write-Host ''
Write-Host 'Sparse MSIX (shell menu; separate developer certificate, informational only):'
foreach ($arch in @('x64', 'x86')) {
    $msix = Join-Path $build "$arch\$Configuration\GlassShellSparse\GlassShellSparse.msix"
    if (-not (Test-Path -LiteralPath $msix -PathType Leaf)) { Write-Host ("  missing  {0}" -f $msix); continue }
    $sig = Get-AuthenticodeSignature -LiteralPath $msix
    $signer = '(none)'
    if ($sig.SignerCertificate) { $signer = ("{0}  {1}" -f $sig.SignerCertificate.Thumbprint, $sig.SignerCertificate.Subject) }
    Write-Host ("  {0,-12} {1}  {2}" -f $sig.Status, $signer, $msix)
}

Write-Host ''
if ($verifyExit -eq 0) {
    Write-Host 'RESULT: all first-party artifacts are signed by the expected certificate.'
}
else {
    Write-Host ("RESULT: FAILED (exit {0}) - see GSIGN errors above. Do not publish this build." -f $verifyExit)
}
exit $verifyExit
