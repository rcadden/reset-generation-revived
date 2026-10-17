# Capture a window (by process name) to PNG. Usage: winshot.ps1 -Proc java -Out file.png
param([string]$Proc = "java", [string]$Out = "shot.png", [string]$Title = "", [int]$ProcId = 0)
Add-Type -AssemblyName System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class W {
  [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool GetClientRect(IntPtr h, out RECT r);
  [DllImport("user32.dll")] public static extern bool ClientToScreen(IntPtr h, ref POINT p);
  [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint f);
  [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
  public struct RECT { public int L, T, R, B; }
  public struct POINT { public int X, Y; }
}
"@
[W]::SetProcessDPIAware() | Out-Null
if ($ProcId -ne 0) { $p = Get-Process -Id $ProcId } else { $p = Get-Process $Proc -ErrorAction Stop | Where-Object { $_.MainWindowHandle -ne 0 -and ($Title -eq "" -or $_.MainWindowTitle -like "*$Title*") } | Select-Object -First 1 }
$h = $p.MainWindowHandle
$r = New-Object W+RECT; [W]::GetClientRect($h, [ref]$r) | Out-Null
$pt = New-Object W+POINT; [W]::ClientToScreen($h, [ref]$pt) | Out-Null
$w = $r.R - $r.L; $hh = $r.B - $r.T
[W]::SetForegroundWindow($h) | Out-Null
Start-Sleep -Milliseconds 200
$bmp = New-Object System.Drawing.Bitmap $w, $hh
$g = [System.Drawing.Graphics]::FromImage($bmp)
# PrintWindow captures the window even when other windows overlap it.
$wr = New-Object W+RECT; [W]::GetWindowRect($h, [ref]$wr) | Out-Null
$full = New-Object System.Drawing.Bitmap ($wr.R - $wr.L), ($wr.B - $wr.T)
$fg = [System.Drawing.Graphics]::FromImage($full)
$hdc = $fg.GetHdc(); [W]::PrintWindow($h, $hdc, 2) | Out-Null; $fg.ReleaseHdc($hdc)
$g.DrawImage($full, (New-Object System.Drawing.Rectangle 0, 0, $w, $hh), ($pt.X - $wr.L), ($pt.Y - $wr.T), $w, $hh, [System.Drawing.GraphicsUnit]::Pixel)
$bmp.Save($Out, [System.Drawing.Imaging.ImageFormat]::Png)
"title=$($p.MainWindowTitle) client=$w x $hh at $($pt.X),$($pt.Y)"
