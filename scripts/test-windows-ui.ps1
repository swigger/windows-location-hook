param(
    [string]$BinaryDirectory = 'x64/Release',
    [string]$OutputDirectory = 'artifacts/ui-check'
)

$ErrorActionPreference = 'Stop'
$output = New-Item -ItemType Directory -Path $OutputDirectory -Force
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
public static class UiCheck {
    public delegate bool EnumProc(IntPtr window, IntPtr data);
    [StructLayout(LayoutKind.Sequential)]
    public struct Rect { public int Left, Top, Right, Bottom; }
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr GetText(IntPtr window, uint message, IntPtr count, StringBuilder text);
    [DllImport("user32.dll")]
    public static extern bool EnumChildWindows(IntPtr parent, EnumProc callback, IntPtr data);
    [DllImport("user32.dll")]
    public static extern IntPtr GetDlgItem(IntPtr parent, int id);
    [DllImport("user32.dll", EntryPoint = "SendMessageW", CharSet = CharSet.Unicode)]
    public static extern IntPtr SetText(IntPtr window, uint message, IntPtr unused, string text);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr SendMessageW(IntPtr window, uint message, IntPtr w, IntPtr l);
    [DllImport("user32.dll")]
    public static extern IntPtr GetClassLongPtrW(IntPtr window, int index);
    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr window, out Rect rect);
    [DllImport("user32.dll")]
    public static extern bool PrintWindow(IntPtr window, IntPtr dc, uint flags);
    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    public static extern uint ExtractIconExW(string path, int index, IntPtr[] large, IntPtr[] small, uint count);
    public static string Text(IntPtr window) {
        var text = new StringBuilder(8192);
        // WM_GETTEXT also retrieves edit/static control text across processes.
        GetText(window, 0x000D, (IntPtr)text.Capacity, text);
        return text.ToString();
    }
    public static string[] AllText(IntPtr window) {
        var result = new List<string> { Text(window) };
        EnumChildWindows(window, (child, data) => { result.Add(Text(child)); return true; }, IntPtr.Zero);
        return result.ToArray();
    }
}
'@

function Assert-Text($window, [string]$expected) {
    if ($expected -notin [UiCheck]::AllText($window)) {
        [UiCheck]::AllText($window) | Write-Output
        throw "Missing or incorrectly encoded UI text: $expected"
    }
    Write-Output "PASS UI text: $expected"
}

function Save-Window($window, [string]$name) {
    $rect = New-Object UiCheck+Rect
    if (-not [UiCheck]::GetWindowRect($window, [ref]$rect)) { throw 'GetWindowRect failed' }
    $bitmap = [Drawing.Bitmap]::new($rect.Right - $rect.Left, $rect.Bottom - $rect.Top)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    try {
        $dc = $graphics.GetHdc()
        try {
            if (-not [UiCheck]::PrintWindow($window, $dc, 2)) { throw 'PrintWindow failed' }
        } finally { $graphics.ReleaseHdc($dc) }
        $bitmap.Save((Join-Path $output.FullName "$name.png"), [Drawing.Imaging.ImageFormat]::Png)
    } finally {
        $graphics.Dispose()
        $bitmap.Dispose()
    }
    [UiCheck]::AllText($window) | Set-Content (Join-Path $output.FullName "$name.txt") -Encoding utf8
}

foreach ($name in @('lfhookcfg', 'LocationDemo')) {
    $exe = (Resolve-Path (Join-Path $BinaryDirectory "$name.exe")).Path
    if ([UiCheck]::ExtractIconExW($exe, -1, $null, $null, 0) -lt 1) {
        throw "No embedded application icon in $name.exe"
    }
    $icon = [Drawing.Icon]::ExtractAssociatedIcon($exe)
    try {
        $bitmap = $icon.ToBitmap()
        try { $bitmap.Save((Join-Path $output.FullName "$name-icon.png")) }
        finally { $bitmap.Dispose() }
    } finally { $icon.Dispose() }

    # The configuration app only opens its UI; do not install a hook or change services.
    $process = Start-Process -FilePath $exe -PassThru
    try {
        $deadline = (Get-Date).AddSeconds(20)
        do {
            Start-Sleep -Milliseconds 200
            $process.Refresh()
            if ($process.HasExited) { throw "$name exited before showing its window" }
            $window = $process.MainWindowHandle
        } while ($window -eq [IntPtr]::Zero -and (Get-Date) -lt $deadline)
        if ($window -eq [IntPtr]::Zero) { throw "$name did not show a window" }
        Start-Sleep -Seconds 2
        Save-Window $window $name
        # Both class icons must be explicitly assigned, including the title-bar size.
        foreach ($index in @(-14, -34)) {
            if ([UiCheck]::GetClassLongPtrW($window, $index) -eq [IntPtr]::Zero) {
                throw "Missing window class icon ($index) in $name"
            }
        }
        Write-Output "PASS embedded, large and small window icons: $name"
        if ($name -eq 'lfhookcfg') {
            Assert-Text $window 'LFHook · 隐私位置配置'
            Assert-Text $window '选择应用将收到的位置与来源'
            Assert-Text $window '保存并应用'
            # Exercise the narrow UTF-8 exception -> UTF-16 status text path as well.
            if ([UiCheck]::SetText([UiCheck]::GetDlgItem($window, 101), 0x000C, [IntPtr]::Zero, 'invalid') -eq [IntPtr]::Zero) {
                throw 'Could not edit latitude'
            }
            [void][UiCheck]::SendMessageW($window, 0x0111, [IntPtr]107, [IntPtr]::Zero)
            Save-Window $window "$name-validation"
            Assert-Text $window '操作失败：请输入有效数字；经纬度使用小数点。'
        } else {
            Assert-Text $window 'Windows 定位服务 Demo — C++'
            Assert-Text $window '获取当前位置'
            Assert-Text $window '尚未请求定位。'
            # Switch mode without requesting the runner's physical location.
            [void][UiCheck]::SendMessageW([UiCheck]::GetDlgItem($window, 1005), 0x014E, [IntPtr]1, [IntPtr]::Zero)
            [void][UiCheck]::SendMessageW($window, 0x0111, [IntPtr](1005 -bor (1 -shl 16)), [IntPtr]::Zero)
            Assert-Text $window '模式已更改，请重新获取位置。'
            Save-Window $window "$name-precise-mode"
        }
        [void]$process.CloseMainWindow()
        if (-not $process.WaitForExit(10000)) { throw "$name did not close normally" }
        if ($process.ExitCode -ne 0) { throw "$name exited with code $($process.ExitCode)" }
    } finally {
        if (-not $process.HasExited) { Stop-Process -Id $process.Id -Force }
        $process.Dispose()
    }
}
