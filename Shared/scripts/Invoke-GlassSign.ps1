#Requires -Version 5.1
<#
.SYNOPSIS
    GlassPlatform Authenticode signing and verification - the single entry point for every sign point.

.DESCRIPTION
    Invoked by Shared\GlassSign.targets after Build/Publish of every first-party artifact and by
    GlassSetup (GlassSetupVerifyEmbeddedMsi) before the payload is embedded. Also used by
    Test-GlassSignatures.ps1 as the release gate. Contract: README.md -> section "... (Authenticode)".

    Keep this file ASCII-only: Windows PowerShell 5.1 reads BOM-less scripts in the ANSI code page.

    Modes
      Sign    Sign each file in -Paths with the author certificate. Files already signed by that
              certificate are skipped, so incremental builds stay cheap.
      Verify  Check that each file carries an intact signature from the expected certificate.

    Policies (GlassSign property)
      Local   Sign/verify when the author key is present in the certificate store; without the key
              only a warning is issued and the build continues unsigned (contributor build).
              With the key present every failure is a hard error - the author must never ship an
              unsigned artifact by accident.
      Require The key (Sign) / the expected signature (Verify) is mandatory.
      Skip    Do nothing.

    Certificate lookup: -Thumbprint (pinned, CurrentUser\My then LocalMachine\My) -> PFX import
    (-PfxPath, password from -PfxPassword or GLASS_SIGN_PFX_PASSWORD) -> newest valid code-signing
    certificate whose Subject equals -Subject. A pinned thumbprint never falls back to the subject.

    Output uses the canonical MSBuild format ("GlassSign : warning GSIGN001: ...") so that Exec
    turns the lines into real warnings/errors. Exit code 1 on any hard failure.

    SignPath perspective: a CI build runs with GlassSign=Skip and the external service signs the
    same artifacts at the same three points (PE -> MSI -> GlassSetup.exe); Verify mode with the
    SignPath thumbprint then serves as the release gate without any change here.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Paths,
    [ValidateSet('Sign', 'Verify')][string]$Mode = 'Sign',
    [ValidateSet('Local', 'Require', 'Skip')][string]$Policy = 'Local',
    [string]$Subject = 'CN=Stanislav K.',
    [string]$Thumbprint = '',
    [string]$PfxPath = '',
    [string]$PfxPassword = '',
    [string]$Description = 'Glass Platform',
    [string]$DescriptionUrl = 'https://github.com/musicstashall/GlassPlatform',
    [string]$SignTool = '',
    [string]$Context = '',
    [switch]$RequireFiles,
    [switch]$NoTimestamp
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$script:Failed = $false
$script:Ctx = ''
if ($Context) { $script:Ctx = " [$Context]" }

$TimestampServers = @(
    'http://timestamp.digicert.com',
    'http://timestamp.sectigo.com',
    'http://timestamp.globalsign.com/tsa/r6advanced1'
)
# Servers that already failed in this invocation are not retried for the next file.
$script:DeadTimestampServers = @()

function Write-GsInfo([string]$Text) {
    Write-Host ("GlassSign:{0} {1}" -f $script:Ctx, $Text)
}

function Write-GsWarn([string]$Code, [string]$Text) {
    Write-Host ("GlassSign : warning {0}: {1}{2}" -f $Code, $Text, $script:Ctx)
}

function Write-GsError([string]$Code, [string]$Text) {
    Write-Host ("GlassSign : error {0}: {1}{2}" -f $Code, $Text, $script:Ctx)
    $script:Failed = $true
}

function Exit-GlassSign {
    if ($script:Failed) { exit 1 }
    exit 0
}

function ConvertTo-NormalizedThumbprint([string]$Value) {
    if (-not $Value) { return '' }
    return (($Value -replace '[^0-9A-Fa-f]', '')).ToUpperInvariant()
}

function Find-SignTool {
    if ($SignTool -and (Test-Path -LiteralPath $SignTool -PathType Leaf)) { return $SignTool }
    if ($env:GLASS_SIGNTOOL -and (Test-Path -LiteralPath $env:GLASS_SIGNTOOL -PathType Leaf)) { return $env:GLASS_SIGNTOOL }

    $roots = @()
    foreach ($pf in @(${env:ProgramFiles(x86)}, $env:ProgramFiles)) {
        if ($pf) { $roots += (Join-Path $pf 'Windows Kits\10\bin') }
    }
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $versions = @(Get-ChildItem -LiteralPath $root -Directory -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -match '^\d+(\.\d+){1,3}$' } |
            Sort-Object { [version]$_.Name } -Descending)
        foreach ($v in $versions) {
            foreach ($arch in @('x64', 'x86')) {
                $candidate = Join-Path $v.FullName "$arch\signtool.exe"
                if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
            }
        }
        foreach ($arch in @('x64', 'x86')) {
            $candidate = Join-Path $root "$arch\signtool.exe"
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { return $candidate }
        }
    }
    $cmd = Get-Command signtool.exe -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($cmd) { return $cmd.Source }
    return $null
}

function Test-CodeSigningEku($Certificate) {
    $eku = $Certificate.Extensions | Where-Object { $_.Oid.Value -eq '2.5.29.37' } | Select-Object -First 1
    if (-not $eku) { return $true }
    foreach ($usage in $eku.EnhancedKeyUsages) {
        if ($usage.Value -eq '1.3.6.1.5.5.7.3.3') { return $true }
    }
    return $false
}

function Get-PrivateKeyCerts([string]$StorePath) {
    if (-not (Test-Path -Path $StorePath)) { return @() }
    return @(Get-ChildItem -Path $StorePath -ErrorAction SilentlyContinue | Where-Object { $_.HasPrivateKey })
}

# Returns @{ Cert = X509Certificate2; Machine = bool } or $null.
function Find-AuthorCert {
    $stores = @('Cert:\CurrentUser\My', 'Cert:\LocalMachine\My')
    $pinned = ConvertTo-NormalizedThumbprint $Thumbprint

    if ($pinned) {
        foreach ($store in $stores) {
            $cert = Get-PrivateKeyCerts $store | Where-Object { $_.Thumbprint -eq $pinned } | Select-Object -First 1
            if ($cert) { return @{ Cert = $cert; Machine = ($store -like 'Cert:\LocalMachine\*') } }
        }
    }

    if ($PfxPath -and (Test-Path -LiteralPath $PfxPath -PathType Leaf)) {
        $password = $PfxPassword
        if (-not $password -and $env:GLASS_SIGN_PFX_PASSWORD) { $password = $env:GLASS_SIGN_PFX_PASSWORD }
        $secure = New-Object System.Security.SecureString
        if ($password) { $secure = ConvertTo-SecureString -String $password -AsPlainText -Force }
        $imported = Import-PfxCertificate -FilePath $PfxPath -CertStoreLocation 'Cert:\CurrentUser\My' -Password $secure -Exportable
        if ($imported) {
            if ($pinned -and $imported.Thumbprint -ne $pinned) {
                Write-GsError 'GSIGN107' ("PFX '{0}' has thumbprint {1}, but GlassSignThumbprint pins {2}." -f $PfxPath, $imported.Thumbprint, $pinned)
                return $null
            }
            return @{ Cert = $imported; Machine = $false }
        }
    }

    $now = Get-Date
    $bySubject = @()
    foreach ($store in $stores) {
        $bySubject += @(Get-PrivateKeyCerts $store | Where-Object { $_.Subject -eq $Subject -and $_.NotAfter -gt $now -and (Test-CodeSigningEku $_) } |
            ForEach-Object { @{ Cert = $_; Machine = ($store -like 'Cert:\LocalMachine\*') } })
    }

    if ($pinned) {
        # A pinned thumbprint never falls back to a different key. A stray key with the author's
        # subject means the pin is stale (or the key was replaced): make that loud on the author's machine.
        if ($bySubject.Count -gt 0) {
            $found = ($bySubject | ForEach-Object { $_.Cert.Thumbprint }) -join ', '
            Write-GsError 'GSIGN107' ("GlassSignThumbprint pins {0}, but only key(s) {1} with subject '{2}' exist. Update the pin in Shared\GlassSign.targets and README.md or remove the stray certificate." -f $pinned, $found, $Subject)
        }
        return $null
    }

    if ($bySubject.Count -eq 0) { return $null }
    return ($bySubject | Sort-Object { $_.Cert.NotBefore } -Descending | Select-Object -First 1)
}

function Get-FileSignature([string]$File) {
    try { return Get-AuthenticodeSignature -LiteralPath $File -ErrorAction Stop }
    catch { return $null }
}

function Test-SignedBy([string]$File, [string]$ExpectedThumbprint) {
    $sig = Get-FileSignature $File
    if (-not $sig -or -not $sig.SignerCertificate) { return $false }
    $status = $sig.Status.ToString()
    if ($status -in @('NotSigned', 'HashMismatch', 'NotSupportedFileFormat', 'Incompatible')) { return $false }
    return ($sig.SignerCertificate.Thumbprint -eq $ExpectedThumbprint)
}

function Invoke-SignTool([string]$Tool, [string[]]$Arguments) {
    # Windows PowerShell turns native stderr into terminating errors under ErrorActionPreference=Stop
    # when 2>&1 is used; relax it for the call only (the exit code is what matters).
    $ErrorActionPreference = 'Continue'
    $output = & $Tool @Arguments 2>&1
    $code = $LASTEXITCODE
    $text = ''
    if ($output) { $text = (($output | ForEach-Object { "$_" }) -join [Environment]::NewLine).Trim() }
    return @{ Code = $code; Output = $text }
}

function Invoke-SignFile([string]$Tool, $Author, [string]$File) {
    $base = @('sign', '/fd', 'SHA256', '/sha1', $Author.Cert.Thumbprint)
    if ($Author.Machine) { $base += '/sm' }
    if ($Description) { $base += @('/d', $Description) }
    if ($DescriptionUrl) { $base += @('/du', $DescriptionUrl) }

    $last = $null
    if (-not $NoTimestamp) {
        foreach ($server in $TimestampServers) {
            if ($script:DeadTimestampServers -contains $server) { continue }
            $last = Invoke-SignTool $Tool ($base + @('/td', 'SHA256', '/tr', $server, $File))
            if ($last.Code -eq 0) {
                Write-GsInfo ("signed (timestamp {0}): {1}" -f $server, $File)
                return $true
            }
            $script:DeadTimestampServers += $server
        }
    }

    $plain = Invoke-SignTool $Tool ($base + @($File))
    if ($plain.Code -eq 0) {
        if ($NoTimestamp) {
            Write-GsInfo ("signed without timestamp (-NoTimestamp): {0}" -f $File)
        }
        else {
            Write-GsWarn 'GSIGN002' ("signed WITHOUT timestamp (no timestamp server reachable; the signature expires with the certificate): {0}" -f $File)
        }
        return $true
    }

    $detail = $plain.Output
    if (-not $detail -and $last) { $detail = $last.Output }
    $detail = ($detail -replace '\s+', ' ')
    if ($detail.Length -gt 400) { $detail = $detail.Substring(0, 400) + '...' }
    Write-GsError 'GSIGN103' ("signtool failed for {0} (exit {1}): {2}" -f $File, $plain.Code, $detail)
    return $false
}

try {
    if ($Policy -eq 'Skip') {
        Write-GsInfo 'GlassSign=Skip - nothing to do.'
        Exit-GlassSign
    }

    $requested = @($Paths -split ';' | ForEach-Object { $_.Trim().Trim('"') } | Where-Object { $_ })
    $files = New-Object 'System.Collections.Generic.List[string]'
    foreach ($path in $requested) {
        if (Test-Path -LiteralPath $path -PathType Leaf) {
            $files.Add((Resolve-Path -LiteralPath $path).ProviderPath)
        }
        elseif ($RequireFiles) {
            Write-GsError 'GSIGN105' ("file not found: {0}" -f $path)
        }
        else {
            Write-GsWarn 'GSIGN003' ("file not found, skipped: {0}" -f $path)
        }
    }

    $author = Find-AuthorCert
    if ($script:Failed) { Exit-GlassSign }

    if ($Mode -eq 'Sign') {
        if (-not $author) {
            if ($Policy -eq 'Require') {
                Write-GsError 'GSIGN102' ("GlassSign=Require, but no signing certificate '{0}' (thumbprint '{1}') with a private key was found." -f $Subject, $Thumbprint)
            }
            else {
                $pinText = ''
                if ($Thumbprint) { $pinText = " (pinned thumbprint $Thumbprint)" }
                Write-GsWarn 'GSIGN001' ("author certificate '{0}'{1} not found in CurrentUser\My - artifacts are left UNSIGNED (contributor build)." -f $Subject, $pinText)
            }
            Exit-GlassSign
        }

        if ($files.Count -eq 0) { Exit-GlassSign }

        $tool = Find-SignTool
        if (-not $tool) {
            Write-GsError 'GSIGN101' 'signtool.exe not found (install the Windows 10/11 SDK or set GLASS_SIGNTOOL). The author key is present, refusing to produce unsigned artifacts.'
            Exit-GlassSign
        }

        foreach ($file in $files) {
            if (Test-SignedBy $file $author.Cert.Thumbprint) {
                Write-GsInfo ("already signed: {0}" -f $file)
                continue
            }
            if (-not (Invoke-SignFile $tool $author $file)) { continue }
            if (-not (Test-SignedBy $file $author.Cert.Thumbprint)) {
                Write-GsError 'GSIGN104' ("signature check after signing failed: {0}" -f $file)
            }
        }
        Exit-GlassSign
    }

    # Mode = Verify
    $expected = ConvertTo-NormalizedThumbprint $Thumbprint
    if (-not $expected -and $author) { $expected = $author.Cert.Thumbprint }
    $hard = ($Policy -eq 'Require') -or ($null -ne $author)
    $problems = New-Object 'System.Collections.Generic.List[string]'

    foreach ($file in $files) {
        $sig = Get-FileSignature $file
        $status = 'Unreadable'
        $signer = ''
        $signerSubject = ''
        $stamp = 'no-timestamp'
        if ($sig) {
            $status = $sig.Status.ToString()
            if ($sig.SignerCertificate) {
                $signer = $sig.SignerCertificate.Thumbprint
                $signerSubject = $sig.SignerCertificate.Subject
            }
            if ($sig.TimeStamperCertificate) { $stamp = 'timestamped' }
        }

        $why = ''
        if (-not $sig -or $status -eq 'NotSigned' -or -not $signer) { $why = 'NOT SIGNED' }
        elseif ($status -in @('HashMismatch', 'NotSupportedFileFormat', 'Incompatible')) { $why = "INVALID SIGNATURE ($status)" }
        elseif ($expected -and $signer -ne $expected) { $why = "signed by $signer ($signerSubject), expected $expected" }
        elseif (-not $expected -and $signerSubject -ne $Subject) { $why = "signed by '$signerSubject', expected subject '$Subject' (no thumbprint pin)" }

        if (-not $why) {
            Write-GsInfo ("OK  {0}  {1}  {2}  {3}" -f $signer, $status, $stamp, $file)
        }
        else {
            $problems.Add(("{0}: {1}" -f $why, $file))
        }
    }

    if ($files.Count -gt 0 -and -not $expected) {
        Write-GsWarn 'GSIGN004' 'no GlassSignThumbprint pin and no author key - signatures were checked by subject only.'
    }

    if ($problems.Count -gt 0) {
        if ($hard) {
            foreach ($problem in $problems) { Write-GsError 'GSIGN106' $problem }
            Write-GsError 'GSIGN106' ("{0} of {1} file(s) are not signed by the author certificate. Rebuild the producing projects in Release (x64 phase for *-x64.msi, x86 phase for *-x86.msi) after the key was created, or build with GlassSign=Skip for an explicitly unsigned package." -f $problems.Count, $files.Count)
        }
        else {
            Write-GsWarn 'GSIGN106' ("{0} of {1} file(s) are not signed by the author certificate (unsigned contributor build): {2}" -f $problems.Count, $files.Count, (($problems | ForEach-Object { Split-Path -Leaf ($_ -replace '^.*: ', '') }) -join ', '))
        }
    }
    Exit-GlassSign
}
catch {
    Write-GsError 'GSIGN199' ("unexpected failure: {0}" -f $_.Exception.Message)
    Exit-GlassSign
}
