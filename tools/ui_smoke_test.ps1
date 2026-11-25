param(
    [string]$Exe = (Join-Path $PSScriptRoot '..\build\LanDrop.exe')
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Text;
using System.Collections.Generic;
using System.Runtime.InteropServices;
public static class LanDropUi {
    public delegate bool EnumProc(IntPtr h, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern bool EnumChildWindows(IntPtr p, EnumProc cb, IntPtr l);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetClassName(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern int GetDlgCtrlID(IntPtr h);
    [DllImport("user32.dll")] static extern bool IsWindowEnabled(IntPtr h);
    [DllImport("user32.dll", CharSet=CharSet.Unicode, EntryPoint="SendMessageW")] public static extern IntPtr SendMessageText(IntPtr h, uint m, IntPtr w, string l);
    [DllImport("user32.dll")] public static extern IntPtr SendMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll")] static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] static extern bool PrintWindow(IntPtr h, IntPtr hdc, uint flags);
    [DllImport("user32.dll", CharSet=CharSet.Unicode)] static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);

    public static List<IntPtr> WindowsFor(uint pid) {
        var list = new List<IntPtr>();
        EnumWindows((h, l) => { uint p; GetWindowThreadProcessId(h, out p); if (p == pid) list.Add(h); return true; }, IntPtr.Zero);
        return list;
    }
    public static string Title(IntPtr h) {
        var s = new StringBuilder(512); GetWindowTextW(h, s, s.Capacity); return s.ToString();
    }
    public static string ClassName(IntPtr h) {
        var s = new StringBuilder(256); GetClassName(h, s, s.Capacity); return s.ToString();
    }
    public static IntPtr MainWindow(uint pid) {
        foreach (var h in WindowsFor(pid)) if (ClassName(h) == "LanDropMainWindow") return h;
        return IntPtr.Zero;
    }
    public static IntPtr ChildById(IntPtr parent, int id) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (GetDlgCtrlID(h) == id) { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static IntPtr FirstButton(IntPtr parent) {
        IntPtr found = IntPtr.Zero;
        EnumChildWindows(parent, (h, l) => { if (ClassName(h) == "Button") { found = h; return false; } return true; }, IntPtr.Zero);
        return found;
    }
    public static bool Enabled(IntPtr h) { return IsWindowEnabled(h); }
    public static string ControlText(IntPtr h) {
        var s = new StringBuilder(2048); GetWindowTextW(h, s, s.Capacity); return s.ToString();
    }
    public static int[] WindowSize(IntPtr hwnd) {
        RECT r; if (!GetWindowRect(hwnd, out r)) return null;
        return new int[] { r.Right - r.Left, r.Bottom - r.Top };
    }
    public static bool Print(IntPtr hwnd, IntPtr hdc) { return PrintWindow(hwnd, hdc, 2); }
}
'@

function Dismiss-WarningDialogs([int]$ProcessId) {
    foreach ($h in [LanDropUi]::WindowsFor([uint32]$ProcessId)) {
        if ([LanDropUi]::ClassName($h) -eq '#32770') {
            $button = [LanDropUi]::ChildById($h, 1)
            if ($button -eq [IntPtr]::Zero) { $button = [LanDropUi]::FirstButton($h) }
            if ($button -ne [IntPtr]::Zero) {
                [void][LanDropUi]::SendMessage($button, 0x00F5, [IntPtr]::Zero, [IntPtr]::Zero)
            } else {
                [void][LanDropUi]::PostMessage($h, 0x0010, [IntPtr]::Zero, [IntPtr]::Zero)
            }
        }
    }
}

$exePath = [System.IO.Path]::GetFullPath($Exe)
$buildDir = [System.IO.Path]::GetDirectoryName($exePath)
$sourceDir = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\build'))
$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$source = Join-Path $sourceDir "ui-smoke-$stamp.bin"
$buffer = New-Object byte[] (1024 * 1024)
$rng = [System.Random]::new(20261125)
$rng.NextBytes($buffer)
$file = [System.IO.File]::Open($source, [System.IO.FileMode]::Create,
    [System.IO.FileAccess]::Write, [System.IO.FileShare]::Read)
for ($i = 0; $i -lt 24; $i++) { $file.Write($buffer, 0, $buffer.Length) }
$file.Dispose()

Get-Process LanDrop -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Milliseconds 500

$receiverProcess = $null
$senderProcess = $null
try {
    $receiverProcess = Start-Process -FilePath $exePath -PassThru
    Start-Sleep -Seconds 1
    $senderProcess = Start-Process -FilePath $exePath -ArgumentList ('"' + $source + '"') -PassThru
    Start-Sleep -Seconds 2

    $receiverWindow = [LanDropUi]::MainWindow([uint32]$receiverProcess.Id)
    $senderWindow = [LanDropUi]::MainWindow([uint32]$senderProcess.Id)
    if ($receiverWindow -eq [IntPtr]::Zero -or $senderWindow -eq [IntPtr]::Zero) {
        throw '无法找到 LanDrop 窗口'
    }

    for ($i = 0; $i -lt 20; $i++) {
        Dismiss-WarningDialogs $senderProcess.Id
        Start-Sleep -Milliseconds 250
        $sendButton = [LanDropUi]::ChildById($senderWindow, 1006)
        if ($sendButton -ne [IntPtr]::Zero -and [LanDropUi]::Enabled($sendButton)) { break }
    }
    $sendButton = [LanDropUi]::ChildById($senderWindow, 1006)
    if ($sendButton -eq [IntPtr]::Zero -or -not [LanDropUi]::Enabled($sendButton)) {
        throw '发送按钮未启用，文件参数未进入待发送列表'
    }

    $receiveDir = $null
    $settingsFile = Join-Path $env:APPDATA 'LanDrop\settings.ini'
    if (Test-Path $settingsFile) {
        foreach ($line in Get-Content $settingsFile -Encoding UTF8) {
            if ($line -match '^\s*receive_dir\s*=\s*(.+?)\s*$') {
                $candidate = $Matches[1]
                if ([System.IO.Path]::IsPathRooted($candidate)) {
                    $receiveDir = [System.IO.Path]::GetFullPath($candidate)
                }
            }
        }
    }
    if ([string]::IsNullOrWhiteSpace($receiveDir)) {
        $receiveDir = Join-Path $env:USERPROFILE 'Downloads\LanDrop'
    }
    if (-not (Test-Path $receiveDir)) {
        New-Item -ItemType Directory -Force $receiveDir | Out-Null
    }
    $destination = Join-Path $receiveDir ([System.IO.Path]::GetFileName($source))
    if (Test-Path $destination) { Remove-Item $destination -Force }

    $targetEdit = [LanDropUi]::ChildById($senderWindow, 1001)
    if ($targetEdit -eq [IntPtr]::Zero) { throw '无法找到目标 IP 输入框' }
    [void][LanDropUi]::SendMessageText($targetEdit, 0x000C, [IntPtr]::Zero, '127.0.0.1')
    [void][LanDropUi]::SendMessage($senderWindow, 0x0111, [IntPtr]1006, [IntPtr]::Zero)

    $deadline = (Get-Date).AddSeconds(60)
    while ((Get-Date) -lt $deadline) {
        Dismiss-WarningDialogs $senderProcess.Id
        if (Test-Path $destination) {
            Start-Sleep -Milliseconds 500
            break
        }
        Start-Sleep -Milliseconds 200
    }
    if (-not (Test-Path $destination)) {
        throw '等待接收文件超时'
    }

    $sourceHash = (Get-FileHash $source -Algorithm SHA256).Hash
    $destinationHash = (Get-FileHash $destination -Algorithm SHA256).Hash
    Write-Host "Received: $destination"
    Write-Host "Size:     $((Get-Item $destination).Length) bytes"
    Write-Host "SHA-256 match: $($sourceHash -eq $destinationHash)"

    Start-Sleep -Seconds 1
    function Save-Window([IntPtr]$Handle, [string]$Path) {
        $size = [LanDropUi]::WindowSize($Handle)
        if ($null -eq $size -or $size[0] -le 0 -or $size[1] -le 0) { return $false }
        $bitmap = New-Object System.Drawing.Bitmap([int]$size[0], [int]$size[1])
        $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
        $hdc = $graphics.GetHdc()
        $ok = [LanDropUi]::Print($Handle, $hdc)
        $graphics.ReleaseHdc($hdc)
        $graphics.Dispose()
        if ($ok) { $bitmap.Save($Path, [System.Drawing.Imaging.ImageFormat]::Png) }
        $bitmap.Dispose()
        return $ok
    }
    $receiverShot = Join-Path $buildDir 'ui-receiver.png'
    $senderShot = Join-Path $buildDir 'ui-sender.png'
    [void](Save-Window $receiverWindow $receiverShot)
    [void](Save-Window $senderWindow $senderShot)
    Write-Host "Screenshots: $receiverShot ; $senderShot"

    if ($sourceHash -ne $destinationHash) { throw 'SHA-256 mismatch' }
    Remove-Item $destination -Force
}
finally {
    if ($senderProcess -and -not $senderProcess.HasExited) { $senderProcess.Kill() }
    if ($receiverProcess -and -not $receiverProcess.HasExited) { $receiverProcess.Kill() }
    if (Test-Path $source) { Remove-Item $source -Force }
}
