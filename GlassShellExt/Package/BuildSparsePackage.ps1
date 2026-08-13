param(
    [Parameter(Mandatory = $true)][string]$ProjectDir,
    [Parameter(Mandatory = $true)][string]$OutDir
)

$ErrorActionPreference = 'Stop'
$ProjectDir = [System.IO.Path]::GetFullPath($ProjectDir.TrimEnd('.', '\', '/'))
$OutDir = [System.IO.Path]::GetFullPath($OutDir.TrimEnd('.', '\', '/'))

$packageSrc = Join-Path $ProjectDir 'Package'
$sparseOut = Join-Path $OutDir 'GlassShellSparse'
$assetsDir = Join-Path $sparseOut 'Assets'

New-Item -ItemType Directory -Force -Path $assetsDir | Out-Null

function Write-StubLogoPng {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][int]$Size
    )
    Add-Type -AssemblyName System.Drawing
    $bmp = New-Object System.Drawing.Bitmap $Size, $Size
    try {
        $graphics = [System.Drawing.Graphics]::FromImage($bmp)
        try {
            $graphics.Clear([System.Drawing.Color]::FromArgb(0x20, 0x20, 0x60))
        }
        finally {
            $graphics.Dispose()
        }
        $bmp.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png)
    }
    finally {
        $bmp.Dispose()
    }
}

# Заглушки для схемы AppX; иконки меню — из ресурсов GlassShellExt.dll.
Write-StubLogoPng -Path (Join-Path $assetsDir 'Square44x44Logo.png') -Size 44
Write-StubLogoPng -Path (Join-Path $assetsDir 'Square150x150Logo.png') -Size 150

$utf8NoBom = New-Object System.Text.UTF8Encoding $false
foreach ($pair in @(
        @{ Src = 'AppxManifest.xml'; Dst = 'AppxManifest.xml' },
        @{ Src = 'AppxManifest.UnregisterStub.xml'; Dst = 'AppxManifest.UnregisterStub.xml' }
    )) {
    $manifestSrc = Join-Path $packageSrc $pair.Src
    $manifestDst = Join-Path $sparseOut $pair.Dst
    if (-not (Test-Path -LiteralPath $manifestSrc)) { throw "Manifest not found: $manifestSrc" }
    $manifestText = Get-Content -LiteralPath $manifestSrc -Raw -Encoding UTF8
    [System.IO.File]::WriteAllText($manifestDst, $manifestText, $utf8NoBom)
}

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$makeAppx = $null
$signTool = $null
if (Test-Path $kitsRoot) {
    foreach ($ver in (Get-ChildItem -Path $kitsRoot -Directory | Sort-Object Name -Descending)) {
        $candidateMake = Join-Path $ver.FullName 'x64\makeappx.exe'
        $candidateSign = Join-Path $ver.FullName 'x64\signtool.exe'
        if ((Test-Path $candidateMake) -and -not $makeAppx) { $makeAppx = $candidateMake }
        if ((Test-Path $candidateSign) -and -not $signTool) { $signTool = $candidateSign }
        if ($makeAppx -and $signTool) { break }
    }
}

$msixPath = Join-Path $sparseOut 'GlassShellSparse.msix'
if ($makeAppx) {
    & $makeAppx pack /o /d $sparseOut /p $msixPath /nv | Out-Null
    if ($signTool) {
        $subj = 'CN=GlassProject Dev'
        $myCandidates = @(Get-ChildItem Cert:\CurrentUser\My | Where-Object { $_.Subject -eq $subj -and $_.HasPrivateKey } | Sort-Object NotBefore -Descending)
        if ($myCandidates.Count -eq 0) {
            $created = New-SelfSignedCertificate -Type Custom -Subject $subj -KeyUsage DigitalSignature -FriendlyName 'Glass Shell Dev' -CertStoreLocation Cert:\CurrentUser\My -KeyContainerName 'GlassShellDev' -TextExtension @('2.5.29.37={text}1.3.6.1.5.5.7.3.3')
            $myCandidates = @($created)
        }

        # Предпочитаем cert, который уже доверен в TrustedPeople, чтобы подпись msix совпадала с trust-цепочкой.
        $trustedThumbs = @(Get-ChildItem Cert:\CurrentUser\TrustedPeople -ErrorAction SilentlyContinue | Where-Object { $_.Subject -eq $subj } | ForEach-Object { $_.Thumbprint })
        $cert = $myCandidates | Where-Object { $trustedThumbs -contains $_.Thumbprint } | Select-Object -First 1
        if (-not $cert) {
            $cert = $myCandidates | Select-Object -First 1
        }

        if (-not (Get-ChildItem Cert:\CurrentUser\TrustedPeople | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
            $pwd = ConvertTo-SecureString 'glass' -AsPlainText -Force
            $pfx = Join-Path $env:TEMP 'GlassShellDev.pfx'
            Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $pwd | Out-Null
            Import-PfxCertificate -FilePath $pfx -CertStoreLocation Cert:\CurrentUser\TrustedPeople -Password $pwd | Out-Null
        }
        if (-not (Get-ChildItem Cert:\CurrentUser\TrustedPublisher -ErrorAction SilentlyContinue | Where-Object { $_.Thumbprint -eq $cert.Thumbprint })) {
            $store = New-Object System.Security.Cryptography.X509Certificates.X509Store('TrustedPublisher', 'CurrentUser')
            $store.Open('ReadWrite')
            $store.Add($cert)
            $store.Close()
        }
        Write-Host "[BuildSparsePackage] signing thumbprint=$($cert.Thumbprint)"
        & $signTool sign /fd SHA256 /sha1 $cert.Thumbprint $msixPath | Out-Null
    }
}
