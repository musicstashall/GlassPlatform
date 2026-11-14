param(
    [Parameter(Mandatory=$true)][string]$Source,
    [Parameter(Mandatory=$true)][string]$Destination
)
$ErrorActionPreference = 'Stop'
$sourceRoot = [IO.Path]::GetFullPath($Source).TrimEnd('\')
$destinationRoot = [IO.Path]::GetFullPath($Destination).TrimEnd('\')
if (-not $destinationRoot.EndsWith('\Build\SetupPayload\OpenGlassResources', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Resources staging destination must be the dedicated Build\SetupPayload\OpenGlassResources directory.'
}
if ($sourceRoot.StartsWith($destinationRoot, [StringComparison]::OrdinalIgnoreCase) -or
    $destinationRoot.StartsWith($sourceRoot + '\', [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Resources source must not be inside its staging destination.'
}
function Assert-NoLink([string]$Path) {
    $current = $Path
    while ($current) {
        if (Test-Path -LiteralPath $current) {
            $item = Get-Item -LiteralPath $current -Force
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $current" }
        }
        $parent = Split-Path $current -Parent
        if ($parent -eq $current) { break }
        $current = $parent
    }
}
Assert-NoLink $sourceRoot
Assert-NoLink $destinationRoot
if (-not (Test-Path -LiteralPath $sourceRoot -PathType Container)) { throw "Resources source is missing: $sourceRoot" }
# Enumerate explicitly without following directory links. Validate the entire
# source and old staging tree before removing stale output or copying anything.
function Get-RegularFiles([string]$Root) {
    $pending = [Collections.Generic.Stack[string]]::new()
    $pending.Push($Root)
    while ($pending.Count) {
        foreach ($item in Get-ChildItem -LiteralPath $pending.Pop() -Force) {
            if ($item.Attributes -band [IO.FileAttributes]::ReparsePoint) { throw "Reparse point refused: $($item.FullName)" }
            if ($item.PSIsContainer) { $pending.Push($item.FullName) } else { $item }
        }
    }
}
$files = @(Get-RegularFiles $sourceRoot)
if (-not $files.Count) { throw 'Refusing to produce an empty OpenGlass Resources MSI.' }
# AtlasTheme images use companion .png.Layout text files for atlas geometry.
# Extension matching is case-insensitive; executable payloads remain forbidden.
$allowed = @('.png','.bmp','.jpg','.jpeg','.gif','.webp','.svg','.ico','.dds','.json','.ini','.xml','.txt','.md','.material','.atlas','.layout')
foreach ($file in $files) {
    if ($allowed -notcontains $file.Extension.ToLowerInvariant()) { throw "Not a supported graphics resource: $($file.FullName)" }
}
if (Test-Path -LiteralPath $destinationRoot) {
    $null = @(Get-RegularFiles $destinationRoot)
    Remove-Item -LiteralPath $destinationRoot -Recurse -Force
}
$null = New-Item -ItemType Directory -Path $destinationRoot -Force
foreach ($file in $files) {
    $relative = $file.FullName.Substring($sourceRoot.Length).TrimStart('\')
    $target = Join-Path $destinationRoot $relative
    $null = New-Item -ItemType Directory -Path (Split-Path $target -Parent) -Force
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
