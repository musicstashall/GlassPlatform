# Read-only packaging gate. Never install/load the application or Native.
param(
    [Parameter(Mandatory = $true)][string]$ApplicationPath,
    [Parameter(Mandatory = $true)][string]$ExpectedVersion,
    [Parameter(Mandatory = $true)][string]$MsiVersion
)
$ErrorActionPreference = 'Stop'
function Read-Version([string]$Text) {
    if ($Text -notmatch '^\d+\.\d+\.\d+\.\d+$') { throw "Invalid Settings version: $Text (four numeric fields required)" }
    $value = [version]$Text
    if ($value.Major -gt 255 -or $value.Minor -gt 255 -or $value.Build -gt 65534 -or $value.Revision -gt 65534) {
        throw "Settings version is outside MSI/assembly bounds: $Text"
    }
    return $value
}
$expected = Read-Version $ExpectedVersion
$msi = Read-Version $MsiVersion
if ($expected -ne $msi) { throw "Settings MSI version $MsiVersion differs from application release $ExpectedVersion" }
$file = Get-Item -LiteralPath $ApplicationPath
if ($file.PSIsContainer -or ($file.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
    throw "Invalid Settings application: $ApplicationPath"
}
$actualText = [Diagnostics.FileVersionInfo]::GetVersionInfo($file.FullName).ProductVersion
$actual = Read-Version $actualText
if ($actual -ne $expected) {
    throw "Stale Settings payload: $ApplicationPath has $actualText; expected $ExpectedVersion. Rebuild/publish GlassSettings for this architecture before MSI packaging."
}
Write-Host "Settings release verified: app=$actualText, MSI=$MsiVersion (Native version independent)"
