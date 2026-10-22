using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Media;
using Microsoft.Win32;

namespace AnyVideoDownload;

public partial class MainWindow : Window
{
    private static readonly Dictionary<string, Brush> LogBrushes = new()
    {
        ["info"] = MakeBrush("#222222"),
        ["ok"] = MakeBrush("#1a7f37"),
        ["warn"] = MakeBrush("#b35900"),
        ["error"] = MakeBrush("#c62828"),
        ["probe"] = MakeBrush("#1f4e8c"),
    };

    private Settings _settings;
    private readonly string? _ffmpegDir;
    private Engine _engine = Engine.None;
    private bool _engineKnown;
    private bool _busy;
    private bool _probing;
    private bool _updating;
    private CancellationTokenSource? _cts;

    public MainWindow()
    {
        InitializeComponent();
        Title = $"{Options.AppName} {Options.AppVersion} (C#)";

        _settings = Settings.Load();
        _ffmpegDir = Core.FindFfmpegDir();

        CbContainer.ItemsSource = Options.VideoContainers;
        CbQuality.ItemsSource = Options.VideoQualities.Select(c => c.Label).ToList();
        CbPriority.ItemsSource = Options.Priorities.Select(c => c.Label).ToList();
        CbAudioFormat.ItemsSource = Options.AudioFormats.Append(Options.AudioOriginalLabel).ToList();
        CbBitrate.ItemsSource = Options.AudioBitrates;
        CbBrowser.ItemsSource = Options.Browsers.Select(c => c.Label).ToList();

        LoadSettingsIntoUi();
        UpdateOptionStates();
        RefreshEnvLabel();

        Loaded += async (_, _) =>
        {
            AppendLog($"{Options.AppName} {Options.AppVersion} 已啟動");
            if (_ffmpegDir is null)
                AppendLog("找不到 ffmpeg：影音合併與格式轉換都需要它。安裝方式：" +
                          "winget install Gyan.FFmpeg（裝完重新開啟本程式）", "warn");
            if (!Core.HasDeno())
                AppendLog("提示：未安裝 deno。YouTube 需要 JavaScript 執行環境才能取得完整格式，" +
                          "若 YouTube 下載失敗或畫質選項很少，請執行 winget install DenoLand.Deno", "warn");
            await DetectEngineAsync(firstTime: true);
        };
    }

    private static Brush MakeBrush(string hex)
    {
        var b = new SolidColorBrush((Color)ColorConverter.ConvertFromString(hex));
        b.Freeze();
        return b;
    }

    // ─── 設定 <-> 介面 ──────────────────────────────────────────────────────

    private void LoadSettingsIntoUi()
    {
        var s = _settings;
        (s.Mode == "audio" ? RbAudio : RbVideo).IsChecked = true;
        CbContainer.SelectedItem = Options.VideoContainers.Contains(s.Container) ? s.Container : "mp4";
        CbQuality.SelectedItem = Options.HasLabel(Options.VideoQualities, s.Quality) ? s.Quality : Options.VideoQualities[0].Label;
        CbPriority.SelectedItem = Options.LabelOf(Options.Priorities, s.Priority, Options.Priorities[0].Label);
        CbAudioFormat.SelectedItem = s.AudioFormat == Options.AudioOriginalValue ? Options.AudioOriginalLabel
            : Options.AudioFormats.Contains(s.AudioFormat) ? s.AudioFormat : "mp3";
        CbBitrate.SelectedItem = Options.AudioBitrates.Contains(s.AudioBitrate) ? s.AudioBitrate : "192";
        ChkPlaylist.IsChecked = s.Playlist;
        ChkMeta.IsChecked = s.EmbedMeta;
        ChkThumb.IsChecked = s.EmbedThumb;
        ChkSubs.IsChecked = s.EmbedSubs;
        CbBrowser.SelectedItem = Options.HasLabel(Options.Browsers, s.Browser) ? s.Browser : Options.Browsers[0].Label;
        OutDirBox.Text = s.OutDir;
    }

    private Settings CollectSettings()
    {
        _settings = new Settings
        {
            OutDir = OutDirBox.Text.Trim(),
            Mode = RbAudio.IsChecked == true ? "audio" : "video",
            Container = CbContainer.SelectedItem as string ?? "mp4",
            Quality = CbQuality.SelectedItem as string ?? Options.VideoQualities[0].Label,
            Priority = Options.ValueOf(Options.Priorities, CbPriority.SelectedItem as string ?? "", "quality"),
            AudioFormat = CbAudioFormat.SelectedItem as string is Options.AudioOriginalLabel
                ? Options.AudioOriginalValue : CbAudioFormat.SelectedItem as string ?? "mp3",
            AudioBitrate = CbBitrate.SelectedItem as string ?? "192",
            Playlist = ChkPlaylist.IsChecked == true,
            EmbedMeta = ChkMeta.IsChecked == true,
            EmbedThumb = ChkThumb.IsChecked == true,
            EmbedSubs = ChkSubs.IsChecked == true,
            Browser = CbBrowser.SelectedItem as string ?? Options.Browsers[0].Label,
        };
        return _settings;
    }

    private void Option_Changed(object sender, RoutedEventArgs e) => UpdateOptionStates();

    private void UpdateOptionStates()
    {
        if (!IsInitialized || BtnStart is null) return;  // InitializeComponent 期間就會觸發
        var video = RbVideo.IsChecked == true;
        var idle = !_busy;
        var container = CbContainer.SelectedItem as string ?? "";
        var audioFormat = CbAudioFormat.SelectedItem as string ?? "";

        var original = Options.ValueOf(Options.Priorities, CbPriority.SelectedItem as string ?? "", "quality") == "original";   // 不轉檔：容器由來源決定
        var audioOriginal = audioFormat == Options.AudioOriginalLabel;
        CbContainer.IsEnabled = idle && video && !original;
        CbQuality.IsEnabled = idle && video;
        CbPriority.IsEnabled = idle && video;
        CbAudioFormat.IsEnabled = idle && !video;
        CbBitrate.IsEnabled = idle && !video && !audioOriginal && !Options.LosslessAudio.Contains(audioFormat);

        var target = video ? (original ? "" : container) : audioFormat;
        ChkThumb.IsEnabled = idle && Options.ThumbOk.Contains(target);       // 容器還不知道時（原始品質、原始音訊）不嵌封面
        ChkSubs.IsEnabled = idle && video && (original || Options.SubsOk.Contains(container));
        foreach (UIElement el in new UIElement[] { RbVideo, RbAudio, ChkPlaylist, ChkMeta, CbBrowser, OutDirBox, BtnBrowse })
            el.IsEnabled = idle;
        UrlBox.IsReadOnly = _busy;

        var ready = idle && _engine.Ok;
        BtnStart.IsEnabled = ready;
        BtnProbe.IsEnabled = ready && !_probing;
        BtnUpdate.IsEnabled = idle && _engineKnown && !_updating;
        BtnCapture.IsEnabled = ready;
        BtnCancel.IsEnabled = _busy && _cts is { IsCancellationRequested: false };
    }

    private void RefreshEnvLabel()
    {
        var ytv = !_engineKnown ? "偵測中…" : _engine.Ok ? _engine.Version : "未安裝";
        var ff = _ffmpegDir is null ? "未找到" : "已找到";
        LblEnv.Text = $"yt-dlp {ytv}　｜　ffmpeg {ff}　｜　.NET {Environment.Version}";
    }

    // ─── 下載核心偵測 ───────────────────────────────────────────────────────

    private async Task DetectEngineAsync(bool firstTime)
    {
        _engineKnown = false;
        UpdateOptionStates();
        RefreshEnvLabel();
        // 偵測要實際執行 yt-dlp --version；await 期間視窗照常回應
        _engine = await Core.DetectEngineAsync();
        _engineKnown = true;
        RefreshEnvLabel();
        UpdateOptionStates();

        if (_engine.Ok)
        {
            if (!firstTime) AppendLog($"下載核心 yt-dlp {_engine.Version} 可以使用。", "ok");
            return;
        }
        AppendLog("找不到下載核心 yt-dlp。", "error");
        if (firstTime && MessageBox.Show(this, "尚未安裝下載核心 yt-dlp。\n\n要現在自動安裝嗎？", Options.AppName,
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            await UpdateCoreAsync();
    }

    // ─── 動作 ───────────────────────────────────────────────────────────────

    // 貼上的常常是整段分享文字（抖音、小紅書的「分享」會連標題與表情符號一起複製），從裡面找出網址；
    // 一行裡沒有網址的（例如 yt-dlp 認得的影片代號）原樣保留。
    private List<string> Urls()
    {
        var result = new List<string>();
        foreach (var line in UrlBox.Text.Split('\n').Select(l => l.Trim()))
        {
            if (line.Length == 0 || line.StartsWith('#')) continue;
            var found = Platforms.ExtractUrls(line);
            foreach (var u in found.Count == 0 ? [line] : found.Select(Platforms.NormalizeUrl))
                if (!result.Contains(u)) result.Add(u);
        }
        return result;
    }

    private void PasteUrls_Click(object sender, RoutedEventArgs e)
    {
        var text = Clipboard.ContainsText() ? Clipboard.GetText().Trim() : "";
        if (text.Length == 0) return;
        var existing = UrlBox.Text;
        if (existing.Trim().Length > 0 && !existing.EndsWith('\n')) existing += "\r\n";
        UrlBox.Text = existing + text + "\r\n";
        UrlBox.CaretIndex = UrlBox.Text.Length;
    }

    private void ClearUrls_Click(object sender, RoutedEventArgs e)
    {
        if (!_busy) UrlBox.Clear();
    }

    private void ChooseDir_Click(object sender, RoutedEventArgs e)
    {
        var dlg = new OpenFolderDialog { Title = "選擇儲存位置" };
        if (Directory.Exists(OutDirBox.Text)) dlg.InitialDirectory = OutDirBox.Text;
        if (dlg.ShowDialog(this) == true) OutDirBox.Text = dlg.FolderName;
    }

    private void OpenDir_Click(object sender, RoutedEventArgs e)
    {
        var d = OutDirBox.Text.Trim();
        if (!Directory.Exists(d))
        {
            MessageBox.Show(this, "資料夾還不存在，第一次下載時會自動建立。", Options.AppName);
            return;
        }
        Process.Start(new ProcessStartInfo(d) { UseShellExecute = true });
    }

    private async void Probe_Click(object sender, RoutedEventArgs e)
    {
        var urls = Urls();
        if (urls.Count == 0)
        {
            MessageBox.Show(this, "請先貼上網址。", Options.AppName);
            return;
        }
        AppendLog("解析中：" + urls[0]);
        _probing = true;
        UpdateOptionStates();
        var job = new ProbeJob(urls[0], CollectSettings(), _engine);
        job.Log += AppendLog;
        var (ok, text) = await job.RunAsync();
        _probing = false;
        UpdateOptionStates();
        AppendLog(text, ok ? "probe" : "error");
    }

    private async void Start_Click(object sender, RoutedEventArgs e)
    {
        var urls = Urls();
        if (urls.Count == 0)
        {
            MessageBox.Show(this, "請先貼上至少一個網址。", Options.AppName);
            return;
        }
        var s = CollectSettings();
        if (s.OutDir.Length == 0)
        {
            MessageBox.Show(this, "請選擇儲存位置。", Options.AppName, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        if (_ffmpegDir is null)
        {
            MessageBox.Show(this, "找不到 ffmpeg，無法合併影音或轉換格式。\n\n" +
                                  "請先安裝：winget install Gyan.FFmpeg\n裝完後重新開啟本程式。",
                Options.AppName, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        try
        {
            Directory.CreateDirectory(s.OutDir);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            MessageBox.Show(this, $"無法建立儲存資料夾：\n{ex.Message}", Options.AppName,
                MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        // 下載前的設定防呆（見 Guard.cs）。可能被改成修正後的設定；使用者取消或遇到致命問題就不開始。
        var guardIssues = DownloadGuard.Check(s, DownloadGuard.ProbeOutDir(s.OutDir));
        var guardFatal = guardIssues.Where(i => i.Fatal).Select(DownloadGuard.Describe).ToList();
        if (guardFatal.Count > 0)
        {
            MessageBox.Show(this, string.Join("\n\n", guardFatal), Options.AppName, MessageBoxButton.OK, MessageBoxImage.Error);
            return;
        }
        if (guardIssues.Count > 0)
        {
            var guardText = string.Join("\n\n", guardIssues.Select(i => "• " + DownloadGuard.Describe(i)));
            if (guardIssues.Any(i => i.Fix.Count > 0))
            {
                var answer = MessageBox.Show(this, guardText + "\n\n要自動修正嗎？（修正後保留最高畫質）\n\n是：自動修正並開始　　否：照原設定開始　　取消：不開始",
                    Options.AppName, MessageBoxButton.YesNoCancel, MessageBoxImage.Question, MessageBoxResult.Yes);
                if (answer == MessageBoxResult.Cancel) return;
                if (answer == MessageBoxResult.Yes)
                {
                    s = DownloadGuard.ApplyFixes(s, guardIssues);
                    _settings = s;
                    LoadSettingsIntoUi();
                    UpdateOptionStates();
                }
            }
            else if (MessageBox.Show(this, guardText + "\n\n要照原設定開始嗎？", Options.AppName, MessageBoxButton.OKCancel, MessageBoxImage.Question) != MessageBoxResult.OK)
            {
                return;
            }
        }
        s.Save();

        _cts = new CancellationTokenSource();
        SetBusy(true);
        var prio = s.Priority == "compat" ? "相容優先" : s.Priority == "original" ? "原始品質（不轉檔）" : "畫質優先";
        var what = s.Mode == "video"
            ? (s.Priority == "original" ? $"影片 {s.Quality} / {prio}" : $"影片 {s.Container} / {s.Quality} / {prio}")
            : s.AudioFormat == Options.AudioOriginalValue ? "音訊 原始格式（不轉檔）"
            : $"音訊 {s.AudioFormat}" + (Options.LosslessAudio.Contains(s.AudioFormat) ? "" : $" / {s.AudioBitrate} kbps");
        AppendLog($"開始下載 {urls.Count} 個網址（{what}）→ {s.OutDir}");

        var job = new DownloadJob(urls, s, _engine, _ffmpegDir);
        _lastJob = job;
        job.Log += AppendLog;
        job.Item += (i, n, url) =>
        {
            LblItem.Text = $"第 {i}/{n} 個：{url}";
            SetBar(0);
            LblStatus.Text = "解析網址中…";
        };
        job.Progress += (pct, text) =>
        {
            SetBar(pct);
            LblStatus.Text = text;
        };
        job.Status += text =>
        {
            SetBar(-1);  // 後處理沒有百分比，改用來回跑的進度條
            LblStatus.Text = text;
        };

        var r = await job.RunAsync(_cts.Token);
        _cts.Dispose();
        _cts = null;
        OnDone(r);
    }

    private void Cancel_Click(object sender, RoutedEventArgs e)
    {
        if (_cts is null) return;
        LblStatus.Text = "取消中…";
        _cts.Cancel();
        UpdateOptionStates();
    }

    private async void UpdateCore_Click(object sender, RoutedEventArgs e) => await UpdateCoreAsync();

    private async Task UpdateCoreAsync()
    {
        AppendLog("更新下載核心 yt-dlp …");
        _updating = true;
        UpdateOptionStates();
        var job = new UpdateJob(_engine);
        job.Log += AppendLog;
        var rc = await job.RunAsync();
        _updating = false;
        if (rc == 0)
        {
            AppendLog("下載核心已更新，重新偵測中…", "ok");
            await DetectEngineAsync(firstTime: false);  // 不必重新開啟程式：下次呼叫 yt-dlp 就是新版本
        }
        else
        {
            AppendLog($"更新失敗（結束代碼 {rc}），請看上方訊息。", "error");
            UpdateOptionStates();
        }
    }

    private void SetBusy(bool busy)
    {
        _busy = busy;
        UpdateOptionStates();
        if (!busy) SetBar(0);
    }

    protected override void OnClosing(CancelEventArgs e)
    {
        if (_busy && MessageBox.Show(this, "還在下載中，確定要結束嗎？", Options.AppName,
                MessageBoxButton.YesNo, MessageBoxImage.Question) != MessageBoxResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        _cts?.Cancel();
        CollectSettings().Save();
        base.OnClosing(e);
    }

    // ─── 下載回報 ───────────────────────────────────────────────────────────

    private DownloadJob? _lastJob;

    // 「從瀏覽器擷取」：網站頁面解析不了、但瀏覽器能播時，抓出播放器實際載入的影音，自動挑最高畫質與音質
    private void Capture_Click(object sender, RoutedEventArgs e) => OpenCapture("");

    private void OpenCapture(string startUrl)
    {
        var s = CollectSettings();
        if (s.OutDir.Length == 0)
        {
            MessageBox.Show(this, "請選擇儲存位置。", Options.AppName, MessageBoxButton.OK, MessageBoxImage.Warning);
            return;
        }
        var first = startUrl.Length > 0 ? startUrl : Urls().FirstOrDefault() ?? "";
        new CaptureWindow(_engine, s.OutDir, _ffmpegDir, first) { Owner = this }.ShowDialog();
    }

    private void OnDone(DownloadResult r)
    {
        var retryByCapture = r.Cancelled || _lastJob is null ? [] : _lastJob.CaptureCandidates.ToList();
        SetBusy(false);
        SetBar(!r.Cancelled && r.Fail == 0 ? 100 : 0);
        string msg;
        if (r.Cancelled)
        {
            msg = $"已取消。完成 {r.Ok} 個。未完成的 .part 暫存檔保留在資料夾，下次同網址可接續下載。";
            AppendLog(msg, "warn");
        }
        else
        {
            msg = $"全部結束：成功 {r.Ok} 個" + (r.Fail > 0 ? $"，失敗 {r.Fail} 個（原因見紀錄）" : "");
            AppendLog(msg, r.Fail == 0 ? "ok" : "warn");
        }
        LblItem.Text = "待機";
        LblStatus.Text = msg;

        // 抖音、小紅書、TikTok 的解析器常因網站改版或要求登入而失敗，但瀏覽器能播，就能用「從瀏覽器擷取」抓下來
        if (retryByCapture.Count > 0 && _engine.Ok &&
            MessageBox.Show(this, "下面的網址下載失敗：\n\n" + string.Join("\n", retryByCapture) +
                            "\n\n這類網站常常可以改用「從瀏覽器擷取」：瀏覽器能播，就能抓。要現在開啟嗎？", Options.AppName,
                MessageBoxButton.YesNo, MessageBoxImage.Question) == MessageBoxResult.Yes)
            OpenCapture(retryByCapture[0]);
    }

    private void SetBar(double pct)
    {
        Bar.IsIndeterminate = pct < 0;
        if (pct >= 0) Bar.Value = Math.Clamp(pct, 0, 100);
    }

    private void AppendLog(string text, string tag = "info")
    {
        var brush = LogBrushes.GetValueOrDefault(tag, LogBrushes["info"]);
        var stamp = DateTime.Now.ToString("HH:mm:ss");
        var lines = text.Split('\n');
        for (var i = 0; i < lines.Length; i++)
        {
            var prefix = i == 0 ? $"[{stamp}] " : new string(' ', 11);
            LogBox.Document.Blocks.Add(new Paragraph(new Run(prefix + lines[i].TrimEnd('\r')) { Foreground = brush })
            {
                Margin = new Thickness(0),
            });
        }
        LogBox.ScrollToEnd();
    }

    private void ClearLog_Click(object sender, RoutedEventArgs e) => LogBox.Document.Blocks.Clear();
}
