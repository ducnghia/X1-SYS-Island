$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force -Path (Join-Path $root 'build') | Out-Null
Start-Transcript -Path (Join-Path $root 'build/v081-install.log') -Force
try {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class SysLabelInstall {
 [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
}
'@
    $source = Join-Path $root 'build/X1-SYS-Island-optimized.exe'
    if (-not (Test-Path $source)) { throw 'Built executable not found' }
    if ((Get-Item $source).VersionInfo.FileVersion -ne '0.8.1.0') { throw 'Expected version 0.8.1.0' }
    $sourceHash = (Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash
    foreach ($test in @('test-sensor-optimization.bat', 'test-hover.bat', 'test.bat')) {
        & (Join-Path $PSScriptRoot $test)
        if ($LASTEXITCODE -ne 0) { throw "Validation failed: $test" }
    }
    $hwnd = [SysLabelInstall]::FindWindow('X1SYSIslandClass','X1 SYS Island')
    if ($hwnd -eq [IntPtr]::Zero) { throw 'Running SYS Island window not found; no files changed' }
    [uint32]$islandPid = 0
    [SysLabelInstall]::GetWindowThreadProcessId($hwnd,[ref]$islandPid) | Out-Null
    $process = Get-Process -Id $islandPid
    $target = $process.MainModule.FileName
    if (-not $target -or [IO.Path]::GetFileName($target) -ne 'X1-SYS-Island.exe') { throw 'Cannot identify installed executable safely' }
    $backup = $target + '.backup-' + (Get-Date -Format 'yyyyMMdd-HHmmssfff')
    $targetHash = (Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash
    Copy-Item -LiteralPath $target -Destination $backup
    if ((Get-FileHash -LiteralPath $backup -Algorithm SHA256).Hash -ne $targetHash) { throw 'Backup hash mismatch; executable unchanged' }
    if ((Get-FileHash -LiteralPath $source -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Source changed during validation; executable unchanged' }
    Write-Output "Backup: $backup"
    if (-not [SysLabelInstall]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero)) { throw 'Cannot request graceful shutdown' }
    if (-not $process.WaitForExit(15000)) { throw 'Application did not close; executable unchanged' }
    $launched = $null
    try {
        Copy-Item -LiteralPath $source -Destination $target -Force
        if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $sourceHash) { throw 'Installed executable hash mismatch' }
        $launched = Start-Process -FilePath $target -WorkingDirectory (Split-Path $target -Parent) -PassThru
        $verified = $false
        for ($i = 0; $i -lt 20; $i++) {
            Start-Sleep -Milliseconds 500
            $window = [SysLabelInstall]::FindWindow('X1SYSIslandClass','X1 SYS Island')
            [uint32]$windowPid = 0
            if ($window -ne [IntPtr]::Zero) {
                [SysLabelInstall]::GetWindowThreadProcessId($window,[ref]$windowPid) | Out-Null
                if ($windowPid -eq $launched.Id) { $verified = $true; break }
            }
            if ($launched.HasExited) { break }
        }
        if (-not $verified) { throw 'New application window not verified; backup retained' }
        Write-Output "PASS: Installed and running PID=$($launched.Id) Path=$target"
    } catch {
        if (-not $launched -or $launched.HasExited) {
            Copy-Item -LiteralPath $backup -Destination $target -Force
            if ((Get-FileHash -LiteralPath $target -Algorithm SHA256).Hash -ne $targetHash) { throw 'Restored executable hash mismatch; backup retained' }
            Start-Process -FilePath $target -WorkingDirectory (Split-Path $target -Parent)
            Write-Output 'Restored previous executable'
        }
        throw
    }
} finally {
    Stop-Transcript
}
