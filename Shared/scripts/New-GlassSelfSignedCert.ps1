#Requires -Version 5.1
param(
    [string]$Subject = 'CN=Stanislav K.',
    [int]$YearsValid = 5
)

$eku = New-Object System.Security.Cryptography.X509Certificates.Oid '1.3.6.1.5.5.7.3.3', 'Code Signing'
$cert = New-SelfSignedCertificate -Subject $Subject `
    -FriendlyName 'GlassPlatform Author Code Signing' `
    -KeyAlgorithm RSA -KeyLength 3072 `
    -HashAlgorithm SHA256 `
    -KeyUsage DigitalSignature `
    -TextExtension @("2.5.29.37={text}$($eku.Value)") `
    -NotAfter (Get-Date).AddYears($YearsValid) `
    -CertStoreLocation 'Cert:\CurrentUser\My'

Write-Host "Created certificate: $($cert.Subject)"
Write-Host "Thumbprint (SHA-1): $($cert.Thumbprint)"
$cerPath = Join-Path $PSScriptRoot 'GlassAuthorCodeSigning.cer'
Export-Certificate -Cert $cert -FilePath $cerPath | Out-Null
Write-Host "Public .cer exported to: $cerPath"
