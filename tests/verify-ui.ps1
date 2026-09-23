param([switch]$SkipHover)
$ErrorActionPreference = 'Stop'
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class IslandUI {
  [StructLayout(LayoutKind.Sequential)] public struct Rect { public int Left,Top,Right,Bottom; }
  [StructLayout(LayoutKind.Sequential)] public struct Point { public int X,Y; }
  [DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);
  [DllImport("user32.dll")] public static extern bool GetCursorPos(out Point p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern IntPtr FindWindow(string c,string t);
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out Rect r);
  [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
  [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
  [DllImport("user32.dll")] public static extern void keybd_event(byte k,byte s,uint f,UIntPtr x);
  [DllImport("user32.dll")] public static extern bool SetProcessDpiAwarenessContext(IntPtr c);
}
'@
Add-Type -AssemblyName System.Drawing
[IslandUI]::SetProcessDpiAwarenessContext([IntPtr](-4)) | Out-Null
$originalCursor = New-Object IslandUI+Point
[IslandUI]::GetCursorPos([ref]$originalCursor) | Out-Null
[IslandUI]::SetCursorPos(1000,500) | Out-Null
$exe = Join-Path (Split-Path $PSScriptRoot -Parent) 'X1-SYS-Island.exe'
$process = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru
Start-Sleep -Seconds 4
$hwnd = [IslandUI]::FindWindow('X1SYSIslandClass','X1 SYS Island')
if ($hwnd -eq [IntPtr]::Zero) { throw 'SYS window missing' }
$rect = New-Object IslandUI+Rect
[IslandUI]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
if (($rect.Right-$rect.Left) -ne 560) { throw 'Width must be 560' }
"SYS bounds: $($rect.Left),$($rect.Top) $($rect.Right-$rect.Left)x$($rect.Bottom-$rect.Top)"
$ai = [IslandUI]::FindWindow('X1AIIslandClass','X1 AI Island')
if ($ai -ne [IntPtr]::Zero) {
  $other = New-Object IslandUI+Rect
  [IslandUI]::GetWindowRect($ai,[ref]$other) | Out-Null
  if ($rect.Left -lt $other.Right -and $rect.Right -gt $other.Left -and $rect.Top -lt $other.Bottom -and $rect.Bottom -gt $other.Top) { throw 'Islands overlap' }
  'AI Island present: no overlap'
}
function Toggle-Shortcut {
  foreach ($key in @(0x11,0x10,0x44)) { [IslandUI]::keybd_event($key,0,0,[UIntPtr]::Zero) }
  foreach ($key in @(0x44,0x10,0x11)) { [IslandUI]::keybd_event($key,0,2,[UIntPtr]::Zero) }
  Start-Sleep -Milliseconds 300
}
Toggle-Shortcut
if ([IslandUI]::IsWindowVisible($hwnd)) { throw 'Ctrl+Shift+D did not hide SYS' }
Toggle-Shortcut
if (-not [IslandUI]::IsWindowVisible($hwnd)) { throw 'Ctrl+Shift+D did not show SYS' }
$second = Start-Process -FilePath $exe -WindowStyle Hidden -PassThru
if (-not $second.WaitForExit(5000)) { throw 'Second instance did not exit' }
[IslandUI]::SendMessage($hwnd,0x203,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 300
[IslandUI]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
if (($rect.Bottom-$rect.Top) -ne 128) { throw 'Expanded size incorrect' }
$bitmap = New-Object System.Drawing.Bitmap(($rect.Right-$rect.Left),($rect.Bottom-$rect.Top))
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left,$rect.Top,0,0,$bitmap.Size)
$bitmap.Save((Join-Path (Split-Path $PSScriptRoot -Parent) 'build/verified-expanded.png'))
$graphics.Dispose(); $bitmap.Dispose()
[IslandUI]::SendMessage($hwnd,0x203,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
'PASS: real hotkey hide/show, single instance, expand/collapse, independent window.'
[IslandUI]::PostMessage($hwnd,0x111,[IntPtr]5,[IntPtr]::Zero) | Out-Null
Start-Sleep -Milliseconds 500
$about = [IslandUI]::FindWindow('#32770','About X1 SYS Island')
if ($about -eq [IntPtr]::Zero) { throw 'About resource dialog missing' }
[IslandUI]::GetWindowRect($about,[ref]$rect) | Out-Null
$bitmap = New-Object System.Drawing.Bitmap(($rect.Right-$rect.Left),($rect.Bottom-$rect.Top))
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.CopyFromScreen($rect.Left,$rect.Top,0,0,$bitmap.Size)
$bitmap.Save((Join-Path (Split-Path $PSScriptRoot -Parent) 'build/verified-about.png'))
$graphics.Dispose(); $bitmap.Dispose()
[IslandUI]::PostMessage($about,0x111,[IntPtr]1,[IntPtr]::Zero) | Out-Null
'PASS: About dialog loaded and closed.'
if ($SkipHover) {
  [IslandUI]::SetCursorPos($originalCursor.X,$originalCursor.Y) | Out-Null
  return
}
Start-Sleep -Milliseconds 300
[IslandUI]::GetWindowRect($hwnd,[ref]$rect) | Out-Null
try {
  [IslandUI]::SetCursorPos($rect.Right+40,$rect.Bottom+40) | Out-Null
  [IslandUI]::SendMessage($hwnd,0x401,[IntPtr]::Zero,[IntPtr]::Zero) | Out-Null
  Start-Sleep -Milliseconds 150
  [IslandUI]::SetCursorPos($rect.Left+80,$rect.Top+22) | Out-Null
  $watch = [Diagnostics.Stopwatch]::StartNew()
  Start-Sleep -Milliseconds 650
  if (-not [IslandUI]::IsWindowVisible($hwnd)) { throw 'Hover hid before one second' }
  while ([IslandUI]::IsWindowVisible($hwnd) -and $watch.ElapsedMilliseconds -lt 1800) { Start-Sleep -Milliseconds 25 }
  if ([IslandUI]::IsWindowVisible($hwnd)) { throw 'One-second hover did not hide' }
  "Hover hide after $($watch.ElapsedMilliseconds) ms"
  $watch.Restart()
  Start-Sleep -Milliseconds 4400
  if ([IslandUI]::IsWindowVisible($hwnd)) { throw 'Hover reappeared before five seconds' }
  while (-not [IslandUI]::IsWindowVisible($hwnd) -and $watch.ElapsedMilliseconds -lt 5800) { Start-Sleep -Milliseconds 25 }
  if (-not [IslandUI]::IsWindowVisible($hwnd)) { throw 'Hover did not reappear' }
  "Hover reappeared after $($watch.ElapsedMilliseconds) ms"
  Start-Sleep -Milliseconds 1300
  if (-not [IslandUI]::IsWindowVisible($hwnd)) { throw 'Repeated hover without leaving' }
  # Re-enter, interrupt with a manual show then hide; old reshow must be cancelled.
  [IslandUI]::SetCursorPos($rect.Right+40,$rect.Bottom+40) | Out-Null
  Start-Sleep -Milliseconds 150
  [IslandUI]::SetCursorPos($rect.Left+80,$rect.Top+22) | Out-Null
  Start-Sleep -Milliseconds 1300
  if ([IslandUI]::IsWindowVisible($hwnd)) { throw 'Hover did not rearm after leaving' }
  Toggle-Shortcut
  Toggle-Shortcut
  [IslandUI]::SetCursorPos($rect.Right+40,$rect.Bottom+40) | Out-Null
  Start-Sleep -Milliseconds 5300
  if ([IslandUI]::IsWindowVisible($hwnd)) { throw 'Manual hide was overridden by hover timer' }
  Toggle-Shortcut
  # Drag starts during the hover countdown; it must cancel hiding.
  [IslandUI]::SetCursorPos($rect.Left+80,$rect.Top+22) | Out-Null
  Start-Sleep -Milliseconds 200
  [IslandUI]::SendMessage($hwnd,0x201,[IntPtr]1,[IntPtr](22*65536+80)) | Out-Null
  Start-Sleep -Milliseconds 1300
  if (-not [IslandUI]::IsWindowVisible($hwnd)) { throw 'Island hid during drag' }
  [IslandUI]::SendMessage($hwnd,0x202,[IntPtr]::Zero,[IntPtr](22*65536+80)) | Out-Null
  'PASS: hover 1s / hidden 5s, no repeated hide, re-entry, manual override and drag.'
} finally {
  [IslandUI]::SetCursorPos($originalCursor.X,$originalCursor.Y) | Out-Null
}
