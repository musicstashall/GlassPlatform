param(
    [Parameter(Mandatory = $true)][string]$ProjectDir,
    # Каталог компонентов меню = ExternalLocation sparse-пакета: Build\<arch>\<cfg>\GlassShellSparse\
    # (OutDir проектов GlassShellExt и GlassShellHost). Если передан корень Build\<arch>\<cfg>\,
    # подкаталог GlassShellSparse добавляется.
    [Parameter(Mandatory = $true)][string]$OutDir,
    # Промежуточный каталог для входа makeappx (только манифест + Assets). Пусто — рядом с OutDir.
    [string]$IntDir = ''
)

# Собирает GlassShellSparse.msix (sparse-пакет: AppxManifest.xml + Assets, без бинарников) и кладёт
# рядом с GlassShellExt.dll / GlassShellHost.exe копии манифестов и Assets (loose-регистрация и
# unregister-stub). Подпись msix — не здесь: Shared\GlassSign.targets подписывает его вместе с DLL
# сертификатом автора (GlassSignPaths в GlassShellExt.vcxproj); Subject сертификата = Publisher манифеста.
# Скрипт не создаёт сертификатов и не пишет в хранилища сертификатов.

$ErrorActionPreference = 'Stop'
$ProjectDir = [System.IO.Path]::GetFullPath($ProjectDir.TrimEnd('.', '\', '/'))
$OutDir = [System.IO.Path]::GetFullPath($OutDir.TrimEnd('.', '\', '/'))

$packageSrc = Join-Path $ProjectDir 'Package'
$sparseOut = $OutDir
if ([System.IO.Path]::GetFileName($sparseOut) -ne 'GlassShellSparse') {
    $sparseOut = Join-Path $OutDir 'GlassShellSparse'
}
if ($IntDir) {
    $packStage = Join-Path ([System.IO.Path]::GetFullPath($IntDir.TrimEnd('.', '\', '/'))) 'SparsePack'
}
else {
    $packStage = Join-Path $sparseOut '.pack'
}

# Вход makeappx должен содержать только манифест и Assets: makeappx pack /d упаковывает весь каталог,
# а в $sparseOut лежат DLL/EXE/PDB и прежний msix.
if (Test-Path -LiteralPath $packStage) { Remove-Item -LiteralPath $packStage -Recurse -Force }
New-Item -ItemType Directory -Force -Path (Join-Path $packStage 'Assets') | Out-Null
New-Item -ItemType Directory -Force -Path (Join-Path $sparseOut 'Assets') | Out-Null

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
foreach ($dir in @($packStage, $sparseOut)) {
    Write-StubLogoPng -Path (Join-Path $dir 'Assets\Square44x44Logo.png') -Size 44
    Write-StubLogoPng -Path (Join-Path $dir 'Assets\Square150x150Logo.png') -Size 150
}

$utf8NoBom = New-Object System.Text.UTF8Encoding $false
foreach ($name in @('AppxManifest.xml', 'AppxManifest.UnregisterStub.xml')) {
    $manifestSrc = Join-Path $packageSrc $name
    if (-not (Test-Path -LiteralPath $manifestSrc)) { throw "Manifest not found: $manifestSrc" }
    $manifestText = Get-Content -LiteralPath $manifestSrc -Raw -Encoding UTF8
    [System.IO.File]::WriteAllText((Join-Path $sparseOut $name), $manifestText, $utf8NoBom)
    # В пакет входит только основной манифест; stub регистрируется loose при снятии.
    if ($name -eq 'AppxManifest.xml') {
        [System.IO.File]::WriteAllText((Join-Path $packStage $name), $manifestText, $utf8NoBom)
    }
}

$kitsRoot = Join-Path ${env:ProgramFiles(x86)} 'Windows Kits\10\bin'
$makeAppx = $null
if (Test-Path $kitsRoot) {
    foreach ($ver in (Get-ChildItem -Path $kitsRoot -Directory | Sort-Object Name -Descending)) {
        $candidateMake = Join-Path $ver.FullName 'x64\makeappx.exe'
        if (Test-Path $candidateMake) { $makeAppx = $candidateMake; break }
    }
}

$msixPath = Join-Path $sparseOut 'GlassShellSparse.msix'
if ($makeAppx) {
    if (Test-Path -LiteralPath $msixPath) { Remove-Item -LiteralPath $msixPath -Force }
    & $makeAppx pack /o /d $packStage /p $msixPath /nv | Out-Null
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path -LiteralPath $msixPath)) {
        throw "makeappx pack failed (exit $LASTEXITCODE): $msixPath"
    }
    Write-Host "[BuildSparsePackage] packed $msixPath (signing: GlassSign.targets)"
}
else {
    Write-Host "[BuildSparsePackage] warning: makeappx.exe not found under $kitsRoot - GlassShellSparse.msix not built (Windows SDK required)"
}
