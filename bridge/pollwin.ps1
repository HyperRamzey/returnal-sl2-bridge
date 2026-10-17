# =============================================================================
#  pollwin.ps1 - snapshot dialog / render windows and their owning process for
#                N seconds.  Used to attribute error dialogs during a test run.
#
#  Usage: powershell -File pollwin.ps1 -Seconds 90 -Out <path>
# =============================================================================
param(
    [int]$Seconds = 90,
    [string]$Out = 'C:\Users\admin\AppData\Local\Temp\opencode\bridge\windows.log'
)

Add-Type @"
using System;
using System.Runtime.InteropServices;
using System.Text;
public class Win32W {
    public delegate bool EnumProc(IntPtr hWnd, IntPtr lParam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lParam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr hWnd, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr hWnd, out uint pid);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr hWnd);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hWnd, out RECT r);
    public struct RECT { public int Left, Top, Right, Bottom; }
}
"@

"=== poller start $(Get-Date -Format 'HH:mm:ss') for $Seconds s ===" | Set-Content $Out
$end = (Get-Date).AddSeconds($Seconds)
$seen = @{}

while ((Get-Date) -lt $end) {
    $found = New-Object System.Collections.ArrayList
    $cb = [Win32W+EnumProc]{
        param($h, $l)
        $procId = 0
        [void][Win32W]::GetWindowThreadProcessId($h, [ref]$procId)
        $sb = New-Object System.Text.StringBuilder 512
        [void][Win32W]::GetWindowTextW($h, $sb, 512)
        $cn = New-Object System.Text.StringBuilder 256
        [void][Win32W]::GetClassNameW($h, $cn, 256)
        $t = $sb.ToString()
        $c = $cn.ToString()
        if ($c -eq '#32770' -or $t -match 'Direct3D|Message|Error|Warning|dlss|Streamline|NVIDIA|Encore|Enabler') {
            $pn = '?'
            try { $pn = (Get-Process -Id $procId -ErrorAction Stop).ProcessName } catch { }
            [void]$found.Add(('{0} pid={1} proc={2} class={3} vis={4} title="{5}"' -f (Get-Date -Format 'HH:mm:ss'), $procId, $pn, $c, ([Win32W]::IsWindowVisible($h)), $t))
        }
        return $true
    }
    [void][Win32W]::EnumWindows($cb, [IntPtr]::Zero)
    foreach ($line in $found) {
        $key = $line.Substring(9)
        if (-not $seen.ContainsKey($key)) { $seen[$key] = 0 }
        $seen[$key]++
        # log first sighting and then every 20th repeat to keep the file small
        if ($seen[$key] -eq 1 -or $seen[$key] % 20 -eq 0) { $line | Add-Content $Out }
    }
    Start-Sleep -Milliseconds 1200
}
"=== poller end $(Get-Date -Format 'HH:mm:ss') ===" | Add-Content $Out
Write-Host "poll windows written to $Out"
