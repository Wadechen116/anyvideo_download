# Qt 版與 C# 版「從瀏覽器擷取」視窗的介面測試（UI Automation）。
#
# 真的開啟主視窗，按「從瀏覽器擷取…」，確認擷取視窗開得出來、初始狀態正確（尚未偵測到影音、
# 「開啟瀏覽器」可按、「下載擷取到的影片」與「停止」停用）、能正常關閉。不會開瀏覽器、不連任何網站。
# 設定檔放在暫存資料夾（覆寫 APPDATA），不會動到使用者自己的設定。
# （Python 版是 Tk，沒有 UI Automation：它的視窗由 py\tests 之外的單元檢查建立一次確認。）
#
# 執行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui_capture.ps1 [qt|csharp]
param([string[]]$Targets = @('qt', 'csharp'))

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$root = Split-Path -Parent $PSScriptRoot
$exes = @{
    qt     = Join-Path $root 'qt\build\Release\AnyVideoDownload.exe'
    csharp = Join-Path $root 'csharp\bin\Release\net8.0-windows\AnyVideoDownload.exe'
}
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

# 擷取視窗在 UI Automation 裡可能是桌面最上層（WPF 有時如此），也可能掛在主視窗底下（Qt 的強制回應視窗如此）：兩處都找
function Find-Dialog($mainWin, [int]$pid_) {
    $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ProcessIdProperty, $pid_)
    foreach ($w in $AE::RootElement.FindAll($TS::Children, $cond)) {
        if ($w.Current.Name -like '從瀏覽器擷取影片*') { return $w }
    }
    $winCond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Window)
    foreach ($w in $mainWin.FindAll($TS::Descendants, $winCond)) {
        if ($w.Current.Name -like '從瀏覽器擷取影片*') { return $w }
    }
}

$script:failed = 0
function Check([string]$label, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Output "PASS  $label" } else { Write-Output "FAIL  $label  $detail"; $script:failed++ }
}

foreach ($t in $Targets) {
    Write-Output "── $t ──"
    $exe = $exes[$t]
    if (-not (Test-Path $exe)) { Check "$t 執行檔存在" $false $exe; continue }

    $appdata = Join-Path ([IO.Path]::GetTempPath()) ("avd_uic_" + [guid]::NewGuid().ToString('N'))
    $cfgDir = Join-Path $appdata 'anyvideo_download'
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    $initial = [ordered]@{ out_dir = (Join-Path $appdata 'out'); mode = 'video'; container = 'mp4'; quality = '最佳'; priority = 'quality' }
    [IO.File]::WriteAllText((Join-Path $cfgDir 'settings.json'), ($initial | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

    $psi = New-Object System.Diagnostics.ProcessStartInfo($exe)
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables['APPDATA'] = $appdata
    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $win = Wait-For { $proc.Refresh(); if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $AE::FromHandle($proc.MainWindowHandle) } } 30
        Check "$t 主視窗開啟" ($null -ne $win)
        if ($null -eq $win) { continue }

        $button = Wait-For { $b = Find-ByName $win '從瀏覽器擷取…' $CT::Button; if ($b -and $b.Current.IsEnabled) { $b } } 60
        Check "$t 找到「從瀏覽器擷取…」，偵測完 yt-dlp 後可按" ($null -ne $button)
        if ($null -eq $button) { continue }

        # 擷取視窗是強制回應視窗：Invoke 會等到它關閉才返回，所以交給另一個程序去按，這裡繼續檢查
        $invoker = @"
Add-Type -AssemblyName UIAutomationClient, UIAutomationTypes
`$el = [System.Windows.Automation.AutomationElement]::FromHandle([IntPtr]$($proc.MainWindowHandle.ToInt64()))
`$c = New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty, '從瀏覽器擷取…')
`$el.FindFirst([System.Windows.Automation.TreeScope]::Descendants, `$c).GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
"@
        $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($invoker))
        $childLog = Join-Path $appdata 'invoke.log'
        $child = Start-Process powershell -ArgumentList '-NoProfile', '-EncodedCommand', $enc -WindowStyle Hidden -PassThru -RedirectStandardError $childLog

        $dialog = Wait-For { Find-Dialog $win $proc.Id } 20
        Check "$t 擷取視窗開啟" ($null -ne $dialog) ((Get-Content $childLog -Raw -ErrorAction SilentlyContinue) + '')
        if ($null -ne $dialog) {
            $open = Find-ByName $dialog '開啟瀏覽器' $CT::Button
            $dl = Find-ByName $dialog '下載擷取到的影片' $CT::Button
            $stop = Find-ByName $dialog '停止' $CT::Button
            Check "$t 「開啟瀏覽器」可按" ($null -ne $open -and $open.Current.IsEnabled)
            Check "$t 還沒偵測到影音：「下載擷取到的影片」停用" ($null -ne $dl -and -not $dl.Current.IsEnabled)
            Check "$t 沒在擷取也沒在下載：「停止」停用" ($null -ne $stop -and -not $stop.Current.IsEnabled)
            $texts = @()
            foreach ($el in $dialog.FindAll($TS::Descendants, (New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty, $CT::Text)))) { $texts += $el.Current.Name }
            Check "$t 顯示「尚未偵測到影音」" ($texts -contains '尚未偵測到影音') ($texts -join ' | ')

            $close = Find-ByName $dialog '關閉' $CT::Button
            $close.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
            $gone = Wait-For { if (-not (Find-Dialog $win $proc.Id)) { $true } } 10
            Check "$t 擷取視窗關閉後回到主視窗" ($gone -eq $true)
        }
        if ($child -and -not $child.HasExited) { $null = $child.WaitForExit(10000); if (-not $child.HasExited) { $child.Kill() } }
        $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
        Check "$t 主視窗正常關閉" ($proc.WaitForExit(10000))
    }
    finally {
        if (-not $proc.HasExited) { $proc.Kill() }
        Remove-Item -Recurse -Force $appdata -ErrorAction SilentlyContinue
    }
}

if ($script:failed -eq 0) { Write-Output "`n全部通過" } else { Write-Output "`n失敗 $($script:failed) 項"; exit 1 }
