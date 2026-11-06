# Read-only artifact inspection. Does not install MSI, change registry or publish a release.
param(
    [string]$PackageDirectory = (Join-Path $PSScriptRoot '..\..\Build\GlassSetup\Release'),
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9_.-]{1,128}$')][string]$ReleaseTag,
    [ValidateSet('platform64','platform32','settings64','settings32','openglass')][string[]]$Products = @('platform64','platform32','settings64','settings32','openglass'),
    [string]$BaseManifest,
    [switch]$IncludeSetup,
    [string]$OutputPath
)
$ErrorActionPreference = 'Stop'
if ($ReleaseTag.Contains('..')) { throw 'Release tag cannot contain ..' }
if (@($Products | Select-Object -Unique).Count -ne $Products.Count) { throw 'Duplicate product selection' }
if (-not $BaseManifest -and ($Products.Count -ne 5 -or -not $IncludeSetup)) {
    throw 'First catalog requires all five MSI and -IncludeSetup; partial publication requires -BaseManifest.'
}
$directory = [IO.Path]::GetFullPath($PackageDirectory)
if (-not $OutputPath) { $OutputPath = Join-Path $directory 'version.json' }
$files = @{
    platform64='Glass-x64.msi'; platform32='Glass-x86.msi'; settings64='GlassSettings-x64.msi';
    settings32='GlassSettings-x86.msi'; openglass='OpenGlass-x64.msi'
}
$upgradeSuffix = @{platform64='5010';platform32='5020';settings64='5040';settings32='5050';openglass='5030'}
function Invoke-Com($Object, [string]$Name, [object[]]$Arguments) {
    return $Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::InvokeMethod, $null, $Object, $Arguments)
}
function Read-Com($Object, [string]$Name, [object[]]$Arguments) {
    return $Object.GetType().InvokeMember($Name, [Reflection.BindingFlags]::GetProperty, $null, $Object, $Arguments)
}
function Test-Version([string]$Value) {
    if ($Value -notmatch '^\d+\.\d+\.\d+(\.\d+)?$') { throw "Invalid version: $Value" }
    foreach ($part in $Value.Split('.')) {
        $number = [uint32]0
        if (-not [uint32]::TryParse($part, [ref]$number)) { throw "Invalid version component: $Value" }
    }
}
function New-Asset([string]$File, [string]$Version) {
    Test-Version $Version
    $path = Join-Path $directory $File
    $info = Get-Item -LiteralPath $path
    if ($info.PSIsContainer -or ($info.Attributes -band [IO.FileAttributes]::ReparsePoint) -or $info.Length -lt 512 -or $info.Length -gt 1GB) {
        throw "Invalid package file: $path"
    }
    return [ordered]@{version=$Version;file=$File;url="https://github.com/musicstashall/GlassPlatform/releases/download/$ReleaseTag/$File";
        sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant();size=$info.Length}
}
# Preserve all other product versions/URLs on a partial MSI publication.
$catalog = [ordered]@{}
if ($BaseManifest) {
    $base = Get-Content -LiteralPath $BaseManifest -Raw | ConvertFrom-Json
    foreach ($property in $base.PSObject.Properties) { $catalog[$property.Name] = $property.Value }
}
$entries = @{}
foreach ($entry in @($catalog['products'])) {
    if ($null -eq $entry) { continue }
    if ($entries.ContainsKey($entry.id)) { throw "Duplicate product ID in base catalog: $($entry.id)" }
    $entries[$entry.id] = $entry
}
$installer = New-Object -ComObject WindowsInstaller.Installer
foreach ($id in $Products) {
    $path = Join-Path $directory $files[$id]
    $db = Invoke-Com $installer 'OpenDatabase' @($path, 0)
    $properties = @{}
    foreach ($name in @('ProductVersion','ProductCode','UpgradeCode')) {
        $view = Invoke-Com $db 'OpenView' @("SELECT ``Value`` FROM ``Property`` WHERE ``Property``='$name'")
        Invoke-Com $view 'Execute' @() | Out-Null
        $row = Invoke-Com $view 'Fetch' @()
        if ($null -eq $row) { throw "Missing $name in $path" }
        $properties[$name] = Read-Com $row 'StringData' @(1)
        Invoke-Com $view 'Close' @() | Out-Null
    }
    $expectedUpgrade = 'B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F' + $upgradeSuffix[$id]
    if ([guid]$properties.UpgradeCode -ne [guid]$expectedUpgrade) { throw "Unexpected UpgradeCode: $path" }
    $summary = Read-Com $db 'SummaryInformation' @(0)
    $template = [string](Read-Com $summary 'Property' @(7))
    $expectedArch = if ($id -in @('platform32','settings32')) {'Intel'} else {'x64'}
    if ($template.Split(';')[0] -ne $expectedArch) { throw "Unexpected MSI architecture: $path ($template)" }
    $asset = New-Asset $files[$id] $properties.ProductVersion
    $asset['id'] = $id
    $asset['productCode'] = ([guid]$properties.ProductCode).ToString('B').ToUpperInvariant()
    $asset['upgradeCode'] = ([guid]$properties.UpgradeCode).ToString('B').ToUpperInvariant()
    $asset['packageCode'] = ([guid](Read-Com $summary 'Property' @(9))).ToString('B').ToUpperInvariant()
    $entries[$id] = $asset
}
$catalog['products'] = @($entries.Keys | Sort-Object | ForEach-Object { $entries[$_] })
if ($IncludeSetup) {
    $exe = Join-Path $directory 'GlassSetup.exe'
    $version = [Diagnostics.FileVersionInfo]::GetVersionInfo($exe).ProductVersion
    $catalog['setupPackage'] = New-Asset 'GlassSetup.exe' $version
    $catalog['setup'] = $version
    if (-not $catalog.Contains('glass')) { $catalog['glass'] = $version }
}
# Legacy scalar refers to Platform, never to a newer Settings-only/Setup-only build.
$legacyPlatform = $null
foreach ($id in @('platform64','platform32')) {
    if (-not $entries.ContainsKey($id)) { continue }
    $value = [string]$entries[$id].version
    Test-Version $value
    $normalized = if ($value.Split('.').Count -eq 3) { $value + '.0' } else { $value }
    if ($null -eq $legacyPlatform -or [version]$normalized -gt [version]$legacyPlatform) { $legacyPlatform = $normalized }
}
if ($legacyPlatform) { $catalog['glass'] = $legacyPlatform }
# Do not truncate the previous manifest on an inspection/hash failure.
$fullOutput = [IO.Path]::GetFullPath($OutputPath)
$part = $fullOutput + '.' + [guid]::NewGuid().ToString('N') + '.part'
try {
    [IO.File]::WriteAllText($part, ($catalog | ConvertTo-Json -Depth 12), [Text.UTF8Encoding]::new($false))
    if ([IO.File]::Exists($fullOutput)) { [IO.File]::Replace($part, $fullOutput, $null) }
    else { [IO.File]::Move($part, $fullOutput) }
} finally {
    if ([IO.File]::Exists($part)) { [IO.File]::Delete($part) }
}
Write-Host "Release catalog generated: $fullOutput (inspect/sign/publish artifacts separately)"
