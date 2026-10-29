param(
    [Parameter(Mandatory = $true)][string] $OutPath,
    [Parameter(Mandatory = $true)][string] $FileVersionDot,
    [Parameter(Mandatory = $true)][string] $ProductVersionDot,
    [Parameter(Mandatory = $true)][string] $ModuleName,
    [Parameter(Mandatory = $true)][string] $BinaryExt,
    [Parameter(Mandatory = $true)][string] $FileType
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Convert-DotVersionToComma([string] $Dot) {
    if ([string]::IsNullOrWhiteSpace($Dot)) { return '0,0,0,0' }
    $parts = $Dot.Trim().Split('.')
    if ($parts.Length -lt 4) {
        $padded = @('0', '0', '0', '0')
        for ($i = 0; $i -lt $parts.Length -and $i -lt 4; $i++) { $padded[$i] = $parts[$i] }
        $parts = $padded
    }
    $nums = foreach ($p in $parts[0..3]) {
        if ($p -match '^\d+$') { [int]$p } else { 0 }
    }
    return ($nums -join ',')
}

$dir = Split-Path -Parent $OutPath
if ($dir -and -not (Test-Path -LiteralPath $dir)) {
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
}

$fileComma = Convert-DotVersionToComma $FileVersionDot
$productComma = Convert-DotVersionToComma $ProductVersionDot

$lines = @(
    '#pragma once'
    "#define GLASS_VER_COMMA $fileComma"
    "#define GLASS_VER_DOT $FileVersionDot"
    "#define GLASS_VER_PRODUCT_COMMA $productComma"
    "#define GLASS_VER_PRODUCT_DOT $ProductVersionDot"
    "#define GLASS_VER_MODULE $ModuleName"
    "#define GLASS_VER_EXT $BinaryExt"
    "#define GLASS_VER_FILETYPE $FileType"
)

$utf8 = New-Object System.Text.UTF8Encoding $false
[System.IO.File]::WriteAllLines($OutPath, $lines, $utf8)
