param(
    [Parameter(Mandatory = $true)][string]$StagingDir,
    [Parameter(Mandatory = $true)][string]$OutFile,
    [Parameter(Mandatory = $true)][string]$ComponentGroupId,
    [Parameter(Mandatory = $true)][string]$RootDirectoryId
)

$ErrorActionPreference = 'Stop'
if (-not (Test-Path $StagingDir)) {
    throw "Staging directory not found: $StagingDir"
}

function Get-StableGuid([string]$relative) {
    $hash = [System.Security.Cryptography.SHA1]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($relative))
    $bytes = New-Object byte[] 16
    [Array]::Copy($hash, $bytes, 16)
    $bytes[6] = ($bytes[6] -band 0x0F) -bor 0x50
    $bytes[8] = ($bytes[8] -band 0x3F) -bor 0xA0
    return ([Guid]::new($bytes)).ToString().ToUpperInvariant()
}

function Get-SafeId([string]$prefix, [string]$relative) {
    $hash = [System.BitConverter]::ToString(
        [System.Security.Cryptography.SHA1]::Create().ComputeHash([Text.Encoding]::UTF8.GetBytes($relative))
    ).Replace('-', '')
    return ($prefix + $hash.Substring(0, 16))
}

$stagingFull = (Resolve-Path $StagingDir).Path.TrimEnd('\')
$dirIds = @{ '' = $RootDirectoryId }
$dirNodes = @{
    '' = @{ Id = $RootDirectoryId; Name = ''; Parent = $null; Children = @() }
}

function Register-Directory([string]$relative) {
    if ($dirIds.ContainsKey($relative)) { return $dirIds[$relative] }
    if ($relative -eq '') { return $RootDirectoryId }
    $parentRel = Split-Path $relative -Parent
    if ($null -eq $parentRel) { $parentRel = '' }
    [void](Register-Directory $parentRel)
    $name = Split-Path $relative -Leaf
    $id = Get-SafeId 'dir_' $relative
    $dirIds[$relative] = $id
    $dirNodes[$relative] = @{ Id = $id; Name = $name; Parent = $parentRel; Children = @() }
    if ($parentRel -ne '') {
        $dirNodes[$parentRel].Children += $relative
    } else {
        $dirNodes[''].Children += $relative
    }
    return $id
}

$files = Get-ChildItem -Path $stagingFull -Recurse -File | Sort-Object FullName
foreach ($file in $files) {
    $rel = $file.FullName.Substring($stagingFull.Length).TrimStart('\')
    $parentRel = Split-Path $rel -Parent
    if ($null -eq $parentRel) { $parentRel = '' }
    [void](Register-Directory $parentRel)
}

$dirs = Get-ChildItem -Path $stagingFull -Recurse -Directory | Sort-Object FullName
foreach ($dir in $dirs) {
    $rel = $dir.FullName.Substring($stagingFull.Length).TrimStart('\')
    if ($rel -eq '') { continue }
    [void](Register-Directory $rel)
}

function Write-DirectoryTree([string]$relative, [int]$indent) {
    $pad = ' ' * $indent
    if ($relative -ne '') {
        $node = $dirNodes[$relative]
        $script:dirXml.Add("$pad<Directory Id=`"$($node.Id)`" Name=`"$($node.Name)`">")
        foreach ($child in $node.Children) {
            Write-DirectoryTree $child ($indent + 2)
        }
        $script:dirXml.Add("$pad</Directory>")
    } else {
        foreach ($child in $dirNodes[''].Children) {
            Write-DirectoryTree $child $indent
        }
    }
}

$script:dirXml = New-Object System.Collections.Generic.List[string]
foreach ($child in ($dirNodes[''].Children | Sort-Object -Unique)) {
    Write-DirectoryTree $child 6
}

$compLines = New-Object System.Collections.Generic.List[string]
foreach ($file in $files) {
    $rel = $file.FullName.Substring($stagingFull.Length).TrimStart('\')
    if ($rel -match '^unins000\.(exe|dat)$') { continue }
    $parentRel = Split-Path $rel -Parent
    if ($null -eq $parentRel) { $parentRel = '' }
    $dirId = $dirIds[$parentRel]
    $compId = Get-SafeId 'cmp_' $rel
    $fileId = Get-SafeId 'fil_' $rel
    $compLines.Add("      <Component Id=`"$compId`" Directory=`"$dirId`" Guid=`"*`">")
    $compLines.Add("        <File Id=`"$fileId`" Source=`"$($file.FullName)`" KeyPath=`"yes`" />")
    $compLines.Add('      </Component>')
}

foreach ($dir in $dirs) {
    $rel = $dir.FullName.Substring($stagingFull.Length).TrimStart('\')
    if ($rel -eq '') { continue }
    $hasFiles = Get-ChildItem -Path $dir.FullName -Recurse -File -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($hasFiles) { continue }
    $dirId = $dirIds[$rel]
    $compId = Get-SafeId 'dircmp_' $rel
    $guid = Get-StableGuid $rel
    $compLines.Add("      <Component Id=`"$compId`" Directory=`"$dirId`" Guid=`"$guid`">")
    $compLines.Add('        <CreateFolder />')
    $compLines.Add('      </Component>')
}

$lines = New-Object System.Collections.Generic.List[string]
$lines.Add('<?xml version="1.0" encoding="utf-8"?>')
$lines.Add('<Wix xmlns="http://wixtoolset.org/schemas/v4/wxs">')
$lines.Add('  <Fragment>')
$lines.Add("    <DirectoryRef Id=`"$RootDirectoryId`">")
foreach ($line in $dirXml) { $lines.Add($line) }
$lines.Add('    </DirectoryRef>')
$lines.Add('  </Fragment>')
$lines.Add('  <Fragment>')
$lines.Add("    <ComponentGroup Id=`"$ComponentGroupId`">")
foreach ($line in $compLines) { $lines.Add($line) }
$lines.Add('    </ComponentGroup>')
$lines.Add('  </Fragment>')
$lines.Add('</Wix>')

$outDir = Split-Path $OutFile -Parent
if ($outDir -and -not (Test-Path $outDir)) {
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null
}
[System.IO.File]::WriteAllLines($OutFile, $lines, [Text.UTF8Encoding]::new($false))
Write-Host "Harvest wxs: $OutFile ($($files.Count) files)"
