#Requires -Version 5.1
<#
.SYNOPSIS
    Creates the GlassPlatform author code-signing certificate (self-signed, CurrentUser\My) and
    exports its public part to Shared\certs\DeveloperCodeSigning.cer.

.DESCRIPTION
    Run once on the author's machine (no administrator rights needed). The private key stays in the
    user's certificate store; only the public .cer is meant to be committed. Afterwards pin the
    printed SHA-1 thumbprint in Shared\GlassSign.targets (GlassSignThumbprint) and README.md
    (section "... (Authenticode)") and back up the key with Export-PfxCertificate OUTSIDE the repository.

    Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts in the ANSI code page.
#>
[CmdletBinding()]
param(
    [string]$Subject = 'CN=Stanislav K.',
    [int]$YearsValid = 5,
    [string]$CerOutDir = '',
    [switch]$Force
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

if (-not $CerOutDir) { $CerOutDir = Join-Path (Split-Path -Parent $PSScriptRoot) 'certs' }
$cerPath = Join-Path $CerOutDir 'DeveloperCodeSigning.cer'

function Get-Sha256Fingerprint($Certificate) {
    $sha = [System.Security.Cryptography.SHA256]::Create()
    try { return (($sha.ComputeHash($Certificate.RawData) | ForEach-Object { $_.ToString('X2') }) -join '') }
    finally { $sha.Dispose() }
}

$existing = @(Get-ChildItem -Path 'Cert:\CurrentUser\My' |
    Where-Object { $_.Subject -eq $Subject -and $_.HasPrivateKey -and $_.NotAfter -gt (Get-Date) })
if ($existing.Count -gt 0 -and -not $Force) {
    Write-Host "A certificate '$Subject' with a private key already exists in CurrentUser\My:"
    foreach ($c in $existing) {
        Write-Host ("  SHA-1 thumbprint {0}  valid until {1:yyyy-MM-dd}" -f $c.Thumbprint, $c.NotAfter)
    }
    Write-Host 'Nothing was created. Re-run with -Force to create another key (then re-pin the new thumbprint in Shared\GlassSign.targets and README.md).'
    exit 0
}

$cert = New-SelfSignedCertificate `
    -Subject $Subject `
    -FriendlyName 'GlassPlatform Author Code Signing' `
    -Type CodeSigningCert `
    -KeyAlgorithm RSA -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -KeyUsage DigitalSignature `
    -KeyExportPolicy Exportable `
    -NotAfter (Get-Date).AddYears($YearsValid) `
    -CertStoreLocation 'Cert:\CurrentUser\My'

New-Item -ItemType Directory -Path $CerOutDir -Force | Out-Null
Export-Certificate -Cert $cert -FilePath $cerPath -Type CERT | Out-Null

$fp256 = Get-Sha256Fingerprint $cert

Write-Host ''
Write-Host 'Created GlassPlatform author code-signing certificate'
Write-Host ("  Subject            : {0}" -f $cert.Subject)
Write-Host ("  Valid              : {0:yyyy-MM-dd} .. {1:yyyy-MM-dd}" -f $cert.NotBefore, $cert.NotAfter)
Write-Host ("  SHA-1 thumbprint   : {0}" -f $cert.Thumbprint)
Write-Host ("  SHA-256 fingerprint: {0}" -f $fp256)
Write-Host ("  Public certificate : {0}" -f $cerPath)
Write-Host ''
Write-Host 'Next steps'
Write-Host '  1. Pin the SHA-1 thumbprint: Shared\GlassSign.targets -> <GlassSignThumbprint>, README.md -> section "... (Authenticode)" table.'
Write-Host '  2. Back up the private key OUTSIDE the repository (*.pfx is git-ignored, but keep it off the working tree anyway):'
Write-Host ("     Export-PfxCertificate -Cert Cert:\CurrentUser\My\{0} -FilePath <backup>\DeveloperCodeSigning.pfx -Password (Read-Host -AsSecureString 'PFX password')" -f $cert.Thumbprint)
Write-Host '  3. Rebuild Release x64, then Release x86 (GlassSetup), and run Shared\scripts\Test-GlassSignatures.ps1.'
