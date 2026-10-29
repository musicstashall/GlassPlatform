#Requires -Version 5.1
param(
    [Parameter(Mandatory = $true)][string]$Paths,
    [string]$Subject = 'CN=Stanislav K.',
    [string]$Thumbprint = '',
    [string]$PfxPath = '',
    [string]$PfxPassword = ''
)

function Find-SignTool {
    $kits = "${env:ProgramFiles(x86)}\Windows Kits\10\bin"
    if (-not (Test-Path $kits)) { return $null }
    Get-ChildItem $kits -Directory | Sort-Object Name -Descending | ForEach-Object {
        $candidate = Join-Path $_.FullName 'x64\signtool.exe'
        if (Test-Path $candidate) { return $candidate }
    } | Select-Object -First 1
}

$signtool = Find-SignTool
if (-not $signtool) {
    Write-Warning 'GlassSign: signtool.exe not found — skipping Authenticode signing.'
    exit 0
}

$cert = $null
if ($Thumbprint) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Thumbprint -eq $Thumbprint -and $_.HasPrivateKey } | Select-Object -First 1
}
if (-not $cert -and $PfxPath -and (Test-Path $PfxPath)) {
    $sec = ConvertTo-SecureString $PfxPassword -AsPlainText -Force
    $cert = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation Cert:\CurrentUser\My -Password $sec
}
if (-not $cert) {
    $cert = Get-ChildItem Cert:\CurrentUser\My | Where-Object {
        $_.HasPrivateKey -and $_.Subject -like "*$Subject*"
    } | Sort-Object NotBefore -Descending | Select-Object -First 1
}
if (-not $cert) {
    Write-Warning "GlassSign: certificate '$Subject' not found — skipping Authenticode signing."
    exit 0
}

$files = $Paths -split ';' | ForEach-Object { $_.Trim() } | Where-Object { $_ -and (Test-Path $_) }
if (-not $files) { exit 0 }

$timestamps = @(
    'http://timestamp.digicert.com',
    'http://timestamp.sectigo.com',
    'http://timestamp.globalsign.com'
)

$signed = $false
foreach ($ts in $timestamps) {
    $args = @('sign', '/fd', 'sha256', '/td', 'sha256', '/tr', $ts, '/sha1', $cert.Thumbprint) + $files
    & $signtool @args
    if ($LASTEXITCODE -eq 0) { $signed = $true; break }
}
if (-not $signed) {
    $args = @('sign', '/fd', 'sha256', '/sha1', $cert.Thumbprint) + $files
    & $signtool @args
    if ($LASTEXITCODE -ne 0) {
        Write-Warning 'GlassSign: signtool failed (with and without timestamp).'
    }
}
