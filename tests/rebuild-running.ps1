$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class SysRestart {
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string t);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
}
'@
$hwnd = [SysRestart]::FindWindow('X1SYSIslandClass','X1 SYS Island')
if ($hwnd -ne [IntPtr]::Zero) {
  [uint32]$islandPid = 0
  [SysRestart]::GetWindowThreadProcessId($hwnd,[ref]$islandPid) | Out-Null
  $process = Get-Process -Id $islandPid
  [SysRestart]::PostMessage($hwnd,0x10,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  if (-not $process.WaitForExit(15000)) { throw 'SYS did not close; build cancelled.' }
}
& (Join-Path (Split-Path $PSScriptRoot -Parent) 'build.bat')
if ($LASTEXITCODE -ne 0) { throw 'Build failed' }
& (Join-Path $PSScriptRoot 'verify-ui.ps1') -SkipHover
