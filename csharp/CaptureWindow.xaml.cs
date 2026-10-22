// 「從瀏覽器擷取」視窗：開瀏覽器 → 你播放 → 顯示偵測到幾個影音 → 一鍵下載（自動挑最高畫質與音質）。邏輯在 Capture.cs。
using System.ComponentModel;
using System.Globalization;
using System.IO;
using System.Windows;

namespace AnyVideoDownload;

public partial class CaptureWindow : Window
{
    private readonly Engine _engine;
    private readonly string _outDir;
    private readonly string? _ffmpegDir;
    private readonly Dictionary<string, Candidate> _candidates = [];
    private CaptureSession? _session;
    private CancellationTokenSource? _cts;
    private bool _browserOpen;
    private bool _running;

    public CaptureWindow(Engine engine, string outDir, string? ffmpegDir, string startUrl)
    {
        InitializeComponent();
        (_engine, _outDir, _ffmpegDir) = (engine, outDir, ffmpegDir);
        UrlBox.Text = startUrl;
        UpdateButtons();
    }

    private void Log(string text)
    {
        LogBox.AppendText(DateTime.Now.ToString("HH:mm:ss  ", CultureInfo.InvariantCulture) + text + "\n");
        LogBox.ScrollToEnd();
    }

    private void UpdateButtons()
    {
        BtnDownload.IsEnabled = _candidates.Count > 0 && !_running;
        BtnStop.IsEnabled = _running || _browserOpen;
        BtnOpen.IsEnabled = !_browserOpen;
    }

    private void OpenBrowser_Click(object sender, RoutedEventArgs e)
    {
        _session = new CaptureSession();
        _session.Found += c => Dispatcher.BeginInvoke(() => OnFound(c));
        _session.Status += t => Dispatcher.BeginInvoke(() => { LblStatus.Text = t; Log(t); });
        try
        {
            _session.Start(UrlBox.Text.Trim());
        }
        catch (InvalidOperationException ex)
        {
            LblStatus.Text = ex.Message;
            Log(ex.Message);
            return;
        }
        _browserOpen = true;
        UpdateButtons();
    }

    private void OnFound(Candidate c)
    {
        if (!_candidates.ContainsKey(c.Url))
            Log($"偵測到 [{c.Kind}] {c.Mime} {(c.Size < 0 ? "大小未知" : (c.Size / 1048576.0).ToString("F1", CultureInfo.InvariantCulture) + " MB")} {new Uri(c.Url).Host}");
        _candidates[c.Url] = c;
        LblFound.Text = $"偵測到影音：{_candidates.Count} 個項目";
        UpdateButtons();
    }

    private async void Download_Click(object sender, RoutedEventArgs e)
    {
        if (_running) return;
        _running = true;
        _cts = new CancellationTokenSource();
        UpdateButtons();
        Bar.Value = 0;
        LblStatus.Text = "讀取正在播放的影片…";
        try
        {
            // 先問瀏覽器：現在在播的是哪一支（長度幾秒）。Facebook 等網站會預先載入下一支，要靠長度把它們分開。
            var target = _session is not null && _browserOpen ? await _session.QueryPlayingVideoAsync() : -1;
            Log(target > 0 ? $"正在播放的影片長度：{target.ToString("F1", CultureInfo.InvariantCulture)} 秒" : "讀不到正在播放的影片長度，改用偵測順序判斷");
            var path = await CaptureRunner.DownloadAndMergeAsync(
                [.. _candidates.Values], target, _outDir, _engine, _ffmpegDir,
                t => Dispatcher.BeginInvoke(() => Log(t)),
                p => Dispatcher.BeginInvoke(() => Bar.Value = Math.Clamp(p, 0, 100)),
                t => Dispatcher.BeginInvoke(() => LblStatus.Text = t),
                _cts.Token);
            Bar.Value = path is null ? 0 : 100;
            LblStatus.Text = path is not null ? "完成：" + path : _cts.IsCancellationRequested ? "已停止。" : "沒有下載到任何檔案，原因見下方紀錄。";
        }
        finally
        {
            _running = false;
            _cts.Dispose();
            _cts = null;
            UpdateButtons();
        }
    }

    // 停止：下載中就結束這一輪下載；否則停止監聽瀏覽器
    private void Stop_Click(object sender, RoutedEventArgs e)
    {
        if (_running)
        {
            _cts?.Cancel();
            return;
        }
        if (_session is not null && _browserOpen)
        {
            _session.Stop();
            _browserOpen = false;
            LblStatus.Text = "已停止擷取（瀏覽器視窗可自行關閉）";
        }
        UpdateButtons();
    }

    private void Close_Click(object sender, RoutedEventArgs e) => Close();

    protected override void OnClosing(CancelEventArgs e)
    {
        _cts?.Cancel();
        _session?.Dispose();
        base.OnClosing(e);
    }
}
