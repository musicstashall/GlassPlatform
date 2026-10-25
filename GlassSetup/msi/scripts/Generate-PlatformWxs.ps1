param(
    [Parameter(Mandatory = $true)][string]$MetadataWxi,
    [Parameter(Mandatory = $true)][string]$TemplateWxs,
    [Parameter(Mandatory = $true)][string]$OutWxs
)

$ErrorActionPreference = 'Stop'
$utf8 = New-Object System.Text.UTF8Encoding $false
$defines = [System.IO.File]::ReadAllText($MetadataWxi, $utf8)
$template = [System.IO.File]::ReadAllText($TemplateWxs, $utf8)
if ($template -notmatch '<Wix') { throw "Invalid template: $TemplateWxs" }
$combined = $template -replace '(<Wix[^>]*>)', "`$1`r`n$defines"
$outDir = Split-Path $OutWxs -Parent
if ($outDir -and -not (Test-Path $outDir)) { New-Item -ItemType Directory -Force -Path $outDir | Out-Null }
[System.IO.File]::WriteAllText($OutWxs, $combined, [Text.UTF8Encoding]::new($false))
Write-Host "Platform wxs: $OutWxs"
