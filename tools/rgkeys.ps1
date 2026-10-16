# Send key presses to the game window, then optionally screenshot.
# Usage: rgkeys.ps1 -Keys "RETURN,DOWN,DOWN" [-Hold 80] [-Gap 250] [-Shot out.png]
param([string]$Keys = "", [int]$Hold = 80, [int]$Gap = 300, [string]$Shot = "", [string]$Proc = "java")
Add-Type @"
using System; using System.Runtime.InteropServices;
public class K {
  [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
  [DllImport("user32.dll")] public static extern uint MapVirtualKey(uint code, uint type);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int c);
}
"@
$vk = @{ RETURN=0x0D; ENTER=0x0D; ESC=0x1B; ESCAPE=0x1B; SPACE=0x20; LEFT=0x25; UP=0x26; RIGHT=0x27; DOWN=0x28; BACK=0x08; TAB=0x09; F1=0x70; F2=0x71; F11=0x7A; CTRLR=0xA3; SHIFT=0x10; CTRL=0x11; ALT=0x12 }
$p = Get-Process $Proc | Where-Object { $_.MainWindowHandle -ne 0 } | Select-Object -First 1
[K]::ShowWindow($p.MainWindowHandle, 9) | Out-Null
[K]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
Start-Sleep -Milliseconds 150
if ($Keys -ne "") {
  foreach ($k in $Keys.Split(",")) {
    $k = $k.Trim().ToUpper()
    if ($k -match '^WAIT(\d+)$') { Start-Sleep -Milliseconds ([int]$Matches[1]); continue }
    if ($vk.ContainsKey($k)) { $code = $vk[$k] } elseif ($k.Length -eq 1) { $code = [int][char]$k } else { $code = [Convert]::ToInt32($k, 16) }
    $ext = 0; if ($code -ge 0x21 -and $code -le 0x2E) { $ext = 1 }
    $sc = [byte][K]::MapVirtualKey($code, 0)
    [K]::keybd_event([byte]$code, $sc, $ext, [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $Hold
    [K]::keybd_event([byte]$code, $sc, ($ext -bor 2), [UIntPtr]::Zero)
    Start-Sleep -Milliseconds $Gap
  }
}
if ($Shot -ne "") { & (Join-Path $PSScriptRoot "winshot.ps1") -Proc $Proc -Out $Shot }
