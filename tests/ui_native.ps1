# Qt 版與 C# 版的介面測試（UI Automation）。
#
# 真的開啟視窗，用 Windows 的 UI Automation 讀取與操作控制項，確認：
#   1. 設定檔的值有正確載入到介面（取捨、影片格式）
#   2. 切到「僅音訊」時，影片相關選項停用、音訊選項啟用
#   3. 背景偵測完成後，狀態列顯示 yt-dlp 版本、「開始下載」可以按
#   4. 關閉視窗時設定會存回設定檔
#
# 不會按「開始下載」，也不連任何網站。設定檔放在暫存資料夾（覆寫 APPDATA），
# 不會動到使用者自己的設定。
#
# 執行：powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui_native.ps1 [qt|csharp]
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

# 同名的文字標籤（例如「取捨」）也會被找到，所以要連控制項類型一起比對
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

function Get-ComboText($combo) {
    $vp = $null
    if ($combo.TryGetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern, [ref]$vp)) {
        $v = $vp.Current.Value
        if ($v) { return $v }
    }
    $sp = $null
    if ($combo.TryGetCurrentPattern([System.Windows.Automation.SelectionPattern]::Pattern, [ref]$sp)) {
        $sel = $sp.Current.GetSelection()
        if ($sel.Count -gt 0) { return $sel[0].Current.Name }
    }
    return $null
}

$script:failed = 0
function Check([string]$label, [bool]$ok, [string]$detail = '') {
    if ($ok) { Write-Output "PASS  $label" } else { Write-Output "FAIL  $label  $detail"; $script:failed++ }
}

$compatLabel = '相容優先 — H.264，到哪都能播'

foreach ($t in $Targets) {
    Write-Output "── $t ──"
    $exe = $exes[$t]
    if (-not (Test-Path $exe)) { Check "$t 執行檔存在" $false $exe; continue }

    $appdata = Join-Path ([IO.Path]::GetTempPath()) ("avd_ui_" + [guid]::NewGuid().ToString('N'))
    $cfgDir = Join-Path $appdata 'anyvideo_download'
    New-Item -ItemType Directory -Force $cfgDir | Out-Null
    $cfg = Join-Path $cfgDir 'settings.json'
    $initial = [ordered]@{
        out_dir = (Join-Path $appdata 'out'); mode = 'video'; container = 'mov'; quality = '720p'
        priority = 'compat'; audio_format = 'flac'; audio_bitrate = '192'; playlist = $false
        embed_meta = $true; embed_thumb = $false; embed_subs = $false; browser = '不使用'
    }
    [IO.File]::WriteAllText($cfg, ($initial | ConvertTo-Json), (New-Object Text.UTF8Encoding($false)))

    $psi = New-Object System.Diagnostics.ProcessStartInfo($exe)
    $psi.UseShellExecute = $false
    $psi.EnvironmentVariables['APPDATA'] = $appdata
    $proc = [System.Diagnostics.Process]::Start($psi)
    try {
        $win = Wait-For {
            $proc.Refresh()
            if ($proc.MainWindowHandle -ne [IntPtr]::Zero) { $AE::FromHandle($proc.MainWindowHandle) }
        } 30
        Check "$t 視窗開啟" ($null -ne $win)
        if ($null -eq $win) { continue }
        $suffix = if ($t -eq 'qt') { '(Qt)' } else { '(C#)' }
        Check "$t 標題含 $suffix" ($win.Current.Name -like "*$suffix*") $win.Current.Name

        $priority = Wait-For { Find-ByName $win '取捨' $CT::ComboBox } 10
        $container = Find-ByName $win '影片格式' $CT::ComboBox
        $audioFmt = Find-ByName $win '音訊格式' $CT::ComboBox
        Check "$t 找到下拉選單" ($null -ne $priority -and $null -ne $container -and $null -ne $audioFmt)

        $pText = Get-ComboText $priority
        Check "$t 取捨載入設定值" ($pText -eq $compatLabel) "實際：$pText"
        $cText = Get-ComboText $container
        Check "$t 影片格式載入設定值" ($cText -eq 'mov') "實際：$cText"
        Check "$t 影片模式：取捨啟用" ($priority.Current.IsEnabled)
        Check "$t 影片模式：音訊格式停用" (-not $audioFmt.Current.IsEnabled)

        # 等背景偵測完成：狀態列出現版本號，開始下載可以按
        $envLabel = Wait-For {
            $cond = New-Object System.Windows.Automation.PropertyCondition($AE::ControlTypeProperty,
                [System.Windows.Automation.ControlType]::Text)
            foreach ($el in $win.FindAll($TS::Descendants, $cond)) {
                if ($el.Current.Name -match '^yt-dlp \d{4}\.') { return $el }
            }
        } 60
        Check "$t 狀態列顯示 yt-dlp 版本" ($null -ne $envLabel)
        $start = Find-ByName $win '開始下載' $CT::Button
        Check "$t 偵測完成後開始下載可按" ($null -ne $start -and $start.Current.IsEnabled)

        $audio = Find-ByName $win '僅音訊' $CT::RadioButton
        $audio.GetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern).Select()
        $switched = Wait-For { if (-not $priority.Current.IsEnabled) { $true } } 5
        Check "$t 切到僅音訊：取捨停用" ($switched -eq $true)
        Check "$t 切到僅音訊：音訊格式啟用" ($audioFmt.Current.IsEnabled)
        $bitrate = Find-ByName $win '位元率' $CT::ComboBox
        Check "$t flac 無損：位元率停用" (-not $bitrate.Current.IsEnabled)

        # 關閉視窗 → 設定存檔
        $win.GetCurrentPattern([System.Windows.Automation.WindowPattern]::Pattern).Close()
        Check "$t 視窗正常關閉" ($proc.WaitForExit(10000))
        $saved = Get-Content $cfg -Raw -Encoding UTF8 | ConvertFrom-Json
        Check "$t 存檔：mode = audio" ($saved.mode -eq 'audio') "實際：$($saved.mode)"
        Check "$t 存檔：priority 保留 compat" ($saved.priority -eq 'compat') "實際：$($saved.priority)"
        Check "$t 存檔：container 保留 mov" ($saved.container -eq 'mov') "實際：$($saved.container)"
        Check "$t 存檔：quality 保留 720p" ($saved.quality -eq '720p') "實際：$($saved.quality)"
    }
    finally {
        if (-not $proc.HasExited) { $proc.Kill() }
        Remove-Item -Recurse -Force $appdata -ErrorAction SilentlyContinue
    }
}

if ($script:failed) { Write-Output "`n失敗 $script:failed 項"; exit 1 }
Write-Output "`n全部通過"
