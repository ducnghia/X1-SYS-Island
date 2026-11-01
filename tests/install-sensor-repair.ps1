$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
Set-Location $root
Start-Transcript -Path (Join-Path $root 'build/sensor-repair-install.log') -Force
try {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
        throw 'Administrator access is required to validate PawnIO and reinstall.'
    }
    & (Join-Path $PSScriptRoot 'test.bat') --require-temperature
    if ($LASTEXITCODE -ne 0) { throw 'Live CPU Package/TjMax validation failed; installation cancelled.' }
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class SensorRepairWindow {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
}
'@
    $source = Join-Path $root 'build/X1-SYS-Island-fixed.exe'
    if (-not (Test-Path $source)) { throw 'Build build/X1-SYS-Island-fixed.exe first.' }
    $target = Join-Path $env:LOCALAPPDATA 'Programs/X1 SYS Island/X1-SYS-Island.exe'
    $window = [SensorRepairWindow]::FindWindow('X1SYSIslandClass', 'X1 SYS Island')
    $process = $null
    if ($window -ne [IntPtr]::Zero) {
        [uint32]$islandPid = 0
        [SensorRepairWindow]::GetWindowThreadProcessId($window, [ref]$islandPid) | Out-Null
        $process = Get-Process -Id $islandPid
        $target = $process.MainModule.FileName
    }
    if (-not (Test-Path $target)) { throw 'Installed executable not found.' }
    $backup = $target + '.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
    Copy-Item -LiteralPath $target -Destination $backup
    Write-Output "Backup: $backup"
    if ($process) {
        if (-not [SensorRepairWindow]::PostMessage($window, 0x10, [IntPtr]::Zero, [IntPtr]::Zero)) { throw 'Cannot close installed application.' }
        if (-not $process.WaitForExit(15000)) { throw 'Application did not close; installation cancelled.' }
    }
    try {
        Copy-Item -LiteralPath $source -Destination $target -Force
        $sha = [Security.Cryptography.SHA256]::Create()
        try {
            $expected = [Convert]::ToBase64String($sha.ComputeHash([IO.File]::ReadAllBytes($source)))
            $actual = [Convert]::ToBase64String($sha.ComputeHash([IO.File]::ReadAllBytes($target)))
            if ($expected -ne $actual) { throw 'Installed executable hash mismatch.' }
        } finally { $sha.Dispose() }
        $module = Join-Path (Split-Path $target -Parent) 'IntelMSR.bin'
        if (-not (Test-Path $module)) { Copy-Item (Join-Path $root 'IntelMSR.bin') $module }
    } catch {
        Copy-Item -LiteralPath $backup -Destination $target -Force
        Start-Process -FilePath $target
        throw
    }
    $launched = Start-Process -FilePath $target -WorkingDirectory (Split-Path $target -Parent) -PassThru
    Start-Sleep -Seconds 3
    $window = [SensorRepairWindow]::FindWindow('X1SYSIslandClass', 'X1 SYS Island')
    [uint32]$verifiedPid = 0
    if ($window -ne [IntPtr]::Zero) { [SensorRepairWindow]::GetWindowThreadProcessId($window, [ref]$verifiedPid) | Out-Null }
    if ($verifiedPid -ne $launched.Id -or $launched.HasExited) { throw 'New application window not verified; backup retained.' }
    Write-Output "PASS: Live temperature validated; installed and running PID=$verifiedPid Path=$target"
} finally { Stop-Transcript }
