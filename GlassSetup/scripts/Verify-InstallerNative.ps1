param(
    [Parameter(Mandatory = $true)][string]$X86,
    [Parameter(Mandatory = $true)][string]$X64
)

# Cheap build-time guard against embedding a previously built Native DLL.
# This does NOT load or execute the DLL. Setup later parses the export table;
# the scan protocol handshake runs after the Settings MSI step.
$ErrorActionPreference = 'Stop'
$markers = @(
    ('GlassSettings_InstallerScanProtocolV1' + [char]0),
    ('GlassSettings_ShellExtRegisterSetupV1W' + [char]0)
)

function Test-NativeFile([string]$Path, [uint16]$ExpectedMachine) {
    $info = [System.IO.FileInfo]::new($Path)
    if (-not $info.Exists -or $info.Length -lt 512 -or $info.Length -gt 67108864) {
        throw "Missing, invalid or too large: $Path"
    }
    $bytes = [System.IO.File]::ReadAllBytes($info.FullName)
    if ([BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D) { throw "Not a PE file: $Path" }
    $pe = [BitConverter]::ToUInt32($bytes, 0x3C)
    if ($pe -gt $bytes.Length - 24 -or [BitConverter]::ToUInt32($bytes, [int]$pe) -ne 0x4550) {
        throw "Invalid PE header: $Path"
    }
    if ([BitConverter]::ToUInt16($bytes, [int]$pe + 4) -ne $ExpectedMachine) {
        throw "Wrong architecture: $Path"
    }
    $ascii = [System.Text.Encoding]::ASCII.GetString($bytes)
    foreach ($marker in $markers) {
        if (-not $ascii.Contains($marker)) {
            throw "Missing installer protocol marker (old DLL): $Path"
        }
    }
    Write-Host "Native payload markers OK: $Path"
}

try {
    Test-NativeFile $X86 0x014C
    Test-NativeFile $X64 0x8664
} catch {
    [Console]::Error.WriteLine("GlassSetup Native validation failed: " + $_.Exception.Message)
    [Console]::Error.WriteLine('Rebuild the solution in Release|x64, then Release|x86, before building GlassSetup.')
    exit 1
}
