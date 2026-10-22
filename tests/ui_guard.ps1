# Qt 版與 C# 版「下載前設定防呆」的介面測試（UI Automation）。
#
# 真的開啟主視窗，貼上一個網址，按「開始下載」，確認：
#   1. 設定有問題（相容優先 + avi + 嵌入字幕）：跳出防呆對話框，按「取消」就不開始下載
#   2. 儲存位置在需要管理員權限的地方（C:\Windows\...）：跳出錯誤對話框，不開始下載
# 兩種情況都在按下「開始下載」時就被擋下，所以不會連任何網站、也不會寫任何檔案。
# 設定檔放在暫存資料夾（覆寫 APPDATA），不會動到使用者自己的設定。
#
# 執行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui_guard.ps1 [qt|csharp]
param([string[]]$Targets = @('qt', 'csharp'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes, System.Windows.Forms
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$exes = @{
    qt     = Join-Path $root 'qt\build\Release\AnyVideoDownload.exe'
    csharp = Join-Path $root 'csharp\bin\Release\net8.0-windows\AnyVideoDownload.exe'
}
Add-Type -Namespace Avd -Name Native -MemberDefinition '[System.Runtime.InteropServices.DllImport("user32.dll")] public static extern bool SetForegroundWindow(System.IntPtr hWnd);'
$AE = [System.Windows.Automation.AutomationElement]
$CT = [System.Windows.Automation.ControlType]
$TS = [System.Windows.Automation.TreeScope]

function Find-ByName($parent, [string]$name, $type = $null) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::NameProperty, $name)
    if ($type) {
        $typeCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $type)
        $cond = New-Object System.Windows.Automation.AndCondition($cond, $typeCond)
    }
    return $parent.FindFirst($TS::Descendants, $cond)
}

function Wait-For([scriptblock]$probe, [int]$seconds = 30) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) {
        $r = & $probe
        if ($r) { return $r }
        Start-Sleep -Milliseconds 250
    }
    return $null
}

# 對話框可能是桌面最上層（WPF 的 MessageBox），也可能掛在主視窗底下（Qt 的 QMessageBox）：兩處都找，名稱不是主視窗的那個
function Find-Dialog($mainWin, [int]$procId) {
    $mainName = $mainWin.Current.Name
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $procId)
    foreach ($w in $AE::RootElement.FindAll($TS::Children, $cond)) {
        if ($w.Current.Name -ne $mainName -and $w.Current.Name -like '*AnyVideo*') { return $w }
    }
    $winCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Window)
    foreach ($w in $mainWin.FindAll($TS::Descendants, $winCond)) {
        if ($w.Current.Name -ne $mainName -and $w.Current.Name -like '*AnyVideo*') { return $w }
    }
}

# 對話框裡所有控制項的名稱，不管型別：Qt 的 QMessageBox 是 Text／Button，WPF 的 MessageBox 是 Windows 原生對話框，控制項在 UI Automation 裡都是 Pane
function Get-Texts($el) {
    $t = @()
    foreach ($x in $el.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) { $t += $x.Current.Name }
    return ($t -join "`n")
}

# 按對話框裡的按鈕：能 Invoke 就 Invoke（Qt 的按鈕）。Windows 原生的 MessageBox 按鈕在 UI Automation 裡只是 Pane，沒有 Invoke，
# 就把焦點給對話框後送鍵盤：取消＝Esc（YesNoCancel 的 Esc 就是取消），確定＝Enter（只有一個按鈕）。
function Click-Named($dialog, [string]$name, [string]$key = '{ENTER}') {
    $btn = Find-ByName $dialog $name
    if ($null -eq $btn) { return $false }
    $ip = $null
    if ($btn.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern, [ref]$ip)) { $ip.Invoke(); return $true }
    # 原生對話框的 UI Automation 元素不能 SetFocus，用視窗控制代碼把它帶到最前面
    $null = [Avd.Native]::SetForegroundWindow([IntPtr]$dialog.Current.NativeWindowHandle)
    Start-Sleep -Milliseconds 400
    [System.Windows.Forms.SendKeys]::SendWait($key)
    return $true
}

$script:failed = 0
function Check([string]$label, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Output "PASS  $label" } else { Write-Output "FAIL  $label  $detail"; $script:failed++ }
}

function Press-StartAndGetDialog($proc, $win, [string]$button) {
    # 強制回應的對話框會讓 Invoke 等到它關閉才返回，所以交給另一個程序去按，這裡繼續檢查
    $invoker = @"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
`$el = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$($proc.MainWindowHandle.ToInt64()))
`$c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, '$button')
`$el.FindFirst([System.Windows.Automation.TreeScope]::Descendants, `$c).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invoker))
    $child = Start-Process powershell -ArgumentList '-NoProfile', '-EncodedCommand', $enc -WindowStyle Hidden -PassThru
    $dialog = Wait-For { Find-Dialog $win $proc.Id } 20
    return @($dialog, $child)
}

foreach ($t in $Targets) {
    Write-Output "── $t ──"
    $exe = $exes[$t]
    if (-not (Test-Path $exe)) { Check "$t 執行檔存在" $false $exe; continue }

    foreach ($scenario in @('fixable', 'admin')) {
        $appdata = Join-Path ([IO.Path]::GetTempPath()) ("avd_uig_" + [guid]::NewGuid().ToString('N'))
        $cfgDir = Join-Path $appdata 'anyvideo_download'
        New-Item -ItemType Directory -Force $cfgDir | Out-Null
        $outDir = if ($scenario -eq 'admin') { 'C:\Windows\avd_guard_test' } else { Join-Path $appdata 'out' }
        $initial = [ordered]@{ out_dir = $outDir; mode = 'video'; container = 'avi'; quality = '最佳'; priority = 'compat'; embed_subs = $true; embed_meta = $true }
        [IO.File]::WriteAllText((Join-Path $cfgDir 'settings.json'), ($initial | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

        $psi = New-Object System.Diagnostics.ProcessStartInfo($exe)
        $psi.UseShellExecute = $false
        $psi.EnvironmentVariables['APPDATA'] = $appdata
        $proc = [System.Diagnostics.Process]::Start($psi)
        try {
            $win = Wait-For { $proc.Refresh(); if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $AE::FromHandle($proc.MainWindowHandle) } } 30
            Check "$t/$scenario 主視窗開啟" ($null -ne $win)
            if ($null -eq $win) { continue }
            $start = Wait-For { $b = Find-ByName $win '開始下載' $CT::Button; if ($b -and $b.Current.IsEnabled) { $b } } 60
            Check "$t/$scenario 偵測完 yt-dlp 後「開始下載」可按" ($null -ne $start)
            if ($null -eq $start) { continue }

            # 貼上一個網址（開始下載需要至少一個）：找最大的文字輸入框
            $edits = $win.FindAll($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Edit)))
            # 只挑可編輯的（紀錄區也是大文字框，但它是唯讀的）
            $url = $null
            foreach ($e in $edits) {
                $vp = $null
                if ($e.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$vp) -and -not $vp.Current.IsReadOnly) {
                    if ($null -eq $url -or $e.Current.BoundingRectangle.Height -gt $url.Current.BoundingRectangle.Height) { $url = $e }
                }
            }
            Check "$t/$scenario 找到可編輯的網址框" ($null -ne $url)
            if ($null -eq $url) { continue }
            $url.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('https://example.invalid/video.mp4')

            $r = Press-StartAndGetDialog $proc $win '開始下載'
            $dialog = $r[0]; $child = $r[1]
            Check "$t/$scenario 跳出防呆對話框" ($null -ne $dialog)
            if ($null -ne $dialog) {
                $text = (Get-Texts $dialog) + "`n" + $dialog.Current.Name
                if ($env:AVD_UI_DEBUG) {
                    Write-Output ("DEBUG 對話框：[" + $dialog.Current.Name + "] " + $dialog.Current.ControlType.ProgrammaticName + " class=" + $dialog.Current.ClassName)
                    foreach ($d in $dialog.FindAll($TS::Descendants, [System.Windows.Automation.Condition]::TrueCondition)) { Write-Output ("   - " + $d.Current.ControlType.ProgrammaticName + " [" + $d.Current.Name + "]") }
                }
                if ($scenario -eq 'fixable') {
                    Check "$t/$scenario 說明「相容優先」做不到 H.264" ($text -like '*相容優先*H.264*')
                    Check "$t/$scenario 說明字幕不會被嵌入" ($text -like '*字幕*不會被嵌入*')
                    Check "$t/$scenario 有「自動修正」與「取消」按鈕" (($text -like '*自動修正*') -and ($null -ne (Find-ByName $dialog '取消')))
                    $null = Click-Named $dialog '取消' '{ESC}'
                } else {
                    Check "$t/$scenario 說明儲存位置需要管理員權限" ($text -like '*管理員權限*')
                    $hasOk = ($null -ne (Find-ByName $dialog '確定')) -or ($null -ne (Find-ByName $dialog 'OK'))
                    Check "$t/$scenario 有「確定」按鈕" $hasOk
                    if (-not (Click-Named $dialog '確定')) { $null = Click-Named $dialog 'OK' }
                }
                $gone = Wait-For { if (-not (Find-Dialog $win $proc.Id)) { $true } } 10
                Check "$t/$scenario 對話框關閉" ($gone -eq $true)
                # 被擋下：沒有開始下載（「開始下載」仍可按、「取消」仍停用）
                $startAfter = Find-ByName $win '開始下載' $CT::Button
                $cancelBtn = Find-ByName $win '取消' $CT::Button
                Check "$t/$scenario 沒有開始下載" ($null -ne $startAfter -and $startAfter.Current.IsEnabled -and ($null -eq $cancelBtn -or -not $cancelBtn.Current.IsEnabled))
            }
            if ($child -and -not $child.HasExited) { $null = $child.WaitForExit(10000); if (-not $child.HasExited) { $child.Kill() } }
            $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
            $null = $proc.WaitForExit(10000)
        }
        finally {
            if (-not $proc.HasExited) { $proc.Kill() }
            Remove-Item -Recurse -Force $appdata -ErrorAction SilentlyContinue
        }
    }
}

if ($script:failed -eq 0) { Write-Output "`n全部通過" } else { Write-Output "`n失敗 $($script:failed) 項"; exit 1 }
