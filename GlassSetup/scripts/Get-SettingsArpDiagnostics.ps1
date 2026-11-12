# Read-only. Run after removal while the two stale Settings entries are visible.
# Does not install, repair, delete registrations, restart Settings or change MSI state.
$ErrorActionPreference = 'Stop'
$codes = @('{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5074}', '{B5E8F3A1-4C2D-4E5F-9A0B-1C2D3E4F5075}')
if (-not ('GlassArpDiagnostic' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class GlassArpDiagnostic {
 [DllImport("msi.dll", CharSet=CharSet.Unicode)]
 public static extern int MsiQueryProductState(string code);
}
'@
}
foreach ($code in $codes) {
    "ProductCode=$code MSI state=$([GlassArpDiagnostic]::MsiQueryProductState($code)) (UNKNOWN=-1, ABSENT=2, DEFAULT=5; current caller context)"
    foreach ($hive in @([Microsoft.Win32.RegistryHive]::LocalMachine,[Microsoft.Win32.RegistryHive]::CurrentUser)) {
        foreach ($view in @([Microsoft.Win32.RegistryView]::Registry64,[Microsoft.Win32.RegistryView]::Registry32)) {
            $base = [Microsoft.Win32.RegistryKey]::OpenBaseKey($hive,$view)
            try {
                $key = $base.OpenSubKey("Software\Microsoft\Windows\CurrentVersion\Uninstall\$code",$false)
                if ($null -eq $key) { "$hive/$view : no ARP key"; continue }
                try {
                    "$hive/$view : ARP key present"
                    foreach ($name in @('DisplayName','DisplayVersion','InstallLocation','DisplayIcon','UninstallString','QuietUninstallString','ModifyPath','WindowsInstaller','NoRemove')) {
                        "  $name=$($key.GetValue($name))"
                    }
                } finally { $key.Dispose() }
            } finally { $base.Dispose() }
        }
    }
}
