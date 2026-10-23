// 從瀏覽器擷取 —— 網站頁面解析不了、但瀏覽器能播時使用（例如 Meta Reels、抖音、小紅書）。
//
// 開一個 Chrome／Edge（偵錯模式、獨立設定檔），使用者照常播放，程式透過 Chrome DevTools Protocol 監聽網路請求，找出播放器
// 實際載入的影音網址（m3u8、mpd、mp4…），用 yt-dlp 下載（以 Chrome 身分連線，否則 fbcdn 這類 CDN 會限速），再用 ffmpeg 串流複製
// 合併。有 DRM 保護的內容抓不到可用的資料。
//
// 自動挑最高畫質與最高音質：
//   1. 先問頁面裡正在播放的 <video>：長度幾秒。Facebook 會預先載入下一支影片，要靠長度把它們分開。
//   2. 把偵測到的檔案（最多 12 個）全部下載，用 ffprobe 量每個檔案的解析度、位元率、長度。
//   3. 只留長度和正在播放的影片最接近的那一組（差距容許 0.15 秒，不是 ±0.6 秒：實測 10.4 秒的影片旁邊有 10.0 秒的另一支）。
//   4. 影像取解析度最大的（同解析度取位元率最高），聲音取位元率最高的，合併成 mp4。
// 限制：只能挑「播放器實際請求過的」畫質。播放器的自動畫質切換一開始常常先請求低畫質；要更高畫質，先在網站播放器裡把畫質調高再擷取。
//
// 規則與 qt/src/capture.cpp、py/avd_capture.py 相同（tests/capture_flow.py 三個版本跑同一個離線端對端測試）。
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Net.Sockets;
using System.Net.WebSockets;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace AnyVideoDownload;

public sealed class Candidate
{
    public string Url { get; set; } = "";       // 已去掉 bytestart／byteend（Meta 把一個檔切成很多段請求）
    public string Mime { get; set; } = "";
    public string Referer { get; set; } = "";
    public string Kind { get; set; } = "";      // stream（m3u8／mpd）、video、audio、media
    public long Size { get; set; } = -1;        // 未知為 -1
    public int Seq { get; set; }                // 第幾個被偵測到（越小越早）
}

public static partial class CaptureUtil
{
    public const int MaxFiles = 12;             // 最多下載幾個偵測到的檔案

    /// <summary>Chrome → Edge；找不到回傳 null。</summary>
    public static string? FindBrowser()
    {
        var roots = new[] { Environment.GetEnvironmentVariable("ProgramFiles"), Environment.GetEnvironmentVariable("ProgramFiles(x86)"),
                            Environment.GetEnvironmentVariable("LOCALAPPDATA") };
        foreach (var rel in new[] { @"Google\Chrome\Application\chrome.exe", @"Microsoft\Edge\Application\msedge.exe" })
            foreach (var root in roots)
                if (!string.IsNullOrEmpty(root) && File.Exists(Path.Combine(root, rel))) return Path.Combine(root, rel);
        return null;
    }

    /// <summary>是影音就回傳種類，不是回傳空字串。</summary>
    public static string CandidateKind(string url, string mime)
    {
        var path = Uri.TryCreate(url, UriKind.Absolute, out var u) ? u.AbsolutePath.ToLowerInvariant() : "";
        var m = mime.ToLowerInvariant();
        if (path.EndsWith(".m3u8") || m.Contains("mpegurl")) return "stream";
        if (path.EndsWith(".mpd") || m.Contains("dash+xml")) return "stream";
        // 切片檔單獨拿沒有用：.ts／.m4s 是串流的一小段，要靠 m3u8／mpd 才能組回來
        if (path.EndsWith(".ts") || path.EndsWith(".m4s") || m.Contains("mp2t")) return "";
        if (m.StartsWith("audio/")) return "audio";
        if (m.StartsWith("video/")) return "video";
        if (path.EndsWith(".mp4") || path.EndsWith(".webm") || path.EndsWith(".m4a")) return "media";
        return "";
    }

    /// <summary>Meta 把同一個檔拆成很多 bytestart／byteend 範圍請求；拿掉才是完整檔案。</summary>
    public static string CleanMediaUrl(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u)) return url;
        var kept = u.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries)
            .Where(p => !p.StartsWith("bytestart=") && !p.StartsWith("byteend=") && p != "bytestart" && p != "byteend");
        return new UriBuilder(u) { Query = string.Join("&", kept) }.Uri.AbsoluteUri;
    }

    /// <summary>網址自己帶的長度（Facebook 的 efg 參數有 duration_s）；沒有回傳 -1。</summary>
    public static double UrlDuration(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u)) return -1;
        foreach (var part in u.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))
        {
            if (!part.StartsWith("efg=")) continue;
            try
            {
                var b64 = Uri.UnescapeDataString(part[4..]).Replace('-', '+').Replace('_', '/');
                b64 = b64.PadRight(b64.Length + (4 - b64.Length % 4) % 4, '=');
                var o = JsonNode.Parse(Encoding.UTF8.GetString(Convert.FromBase64String(b64)));
                return o?["duration_s"]?.GetValue<double>() ?? -1;
            }
            catch (Exception e) when (e is FormatException or System.Text.Json.JsonException or InvalidOperationException) { return -1; }
        }
        return -1;
    }

    internal static string Header(JsonNode? headers, string name)
    {
        if (headers is JsonObject o)
            foreach (var kv in o)
                if (string.Equals(kv.Key, name, StringComparison.OrdinalIgnoreCase)) return kv.Value?.ToString() ?? "";
        return "";
    }
}

/// <summary>單一瀏覽器的 DevTools 連線，只負責找出影音網址。事件在背景執行緒觸發，介面要自己轉回介面執行緒。</summary>
public sealed class CaptureSession : IDisposable
{
    private readonly Dictionary<string, Candidate> _seen = [];
    private readonly Dictionary<string, string> _sessions = [];     // sessionId -> 目標類型（page、iframe、worker…）
    private readonly HashSet<int> _pendingEval = [];
    private readonly List<double[]> _videos = [];
    private readonly SemaphoreSlim _sendLock = new(1, 1);
    private readonly CancellationTokenSource _cts = new();
    private readonly object _gate = new();
    private ClientWebSocket? _ws;
    private TaskCompletionSource<bool>? _evalDone;
    private int _nextId;
    private int _getTargetsId;
    private int _port;
    private string _startUrl = "";
    private bool _navigated;

    public event Action<Candidate>? Found;
    public event Action<string>? Status;

    public string Profile { get; }
    /// <summary>上一次 QueryPlayingVideoAsync 讀到的那支是不是真的在播（沒暫停、播了一點）。</summary>
    public bool LastQueryPlaying { get; private set; }

    public IReadOnlyList<Candidate> Candidates { get { lock (_gate) return [.. _seen.Values]; } }

    /// <param name="profile">null：用 AVD_CAPTURE_PROFILE（測試用，不碰你平常的擷取設定檔），再沒有就用 %APPDATA%\anyvideo_download\capture-profile。</param>
    public CaptureSession(string? profile = null)
    {
        Profile = profile ?? Environment.GetEnvironmentVariable("AVD_CAPTURE_PROFILE") ??
                  Path.Combine(Environment.GetEnvironmentVariable("APPDATA") ?? Environment.GetFolderPath(Environment.SpecialFolder.UserProfile),
                               "anyvideo_download", "capture-profile");
    }

    public void Start(string startUrl)
    {
        var browser = CaptureUtil.FindBrowser() ?? throw new InvalidOperationException("找不到 Chrome 或 Edge。");
        var listener = new TcpListener(IPAddress.Loopback, 0);          // 向系統要一個沒人用的埠
        listener.Start();
        _port = ((IPEndPoint)listener.LocalEndpoint).Port;
        listener.Stop();
        _startUrl = startUrl;
        // 獨立的設定檔：不影響你平常用的瀏覽器（Chrome 也規定偵錯模式必須用獨立設定檔）。
        // 要看登入限定的內容，在這個視窗登入一次，下次還在。
        // 用 Shell 啟動（UseShellExecute）：Shell 啟動的程序不會繼承我們的 handle。一般的 CreateProcess 會讓瀏覽器繼承我們所有可繼承的
        // handle，包含呼叫端拿來收標準輸出的管道，呼叫端就要等到瀏覽器關閉才等得到管道結束（測試時會卡住）。
        var psi = new ProcessStartInfo(browser)
        {
            UseShellExecute = true,
            Arguments = $"--remote-debugging-port={_port} \"--user-data-dir={Profile}\" --no-first-run --no-default-browser-check about:blank",
        };
        try
        {
            Process.Start(psi)?.Dispose();
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException)
        {
            throw new InvalidOperationException($"無法啟動瀏覽器：{browser}（{e.Message}）", e);
        }
        Status?.Invoke("正在開啟瀏覽器…");
        _ = Task.Run(RunAsync);
    }

    /// <summary>結束監聽（瀏覽器視窗不關）。</summary>
    public void Stop()
    {
        _cts.Cancel();
        try { _ws?.Abort(); } catch (ObjectDisposedException) { }
    }

    public void Dispose()
    {
        Stop();
        _cts.Dispose();
        _ws?.Dispose();
        _sendLock.Dispose();
    }

    private async Task RunAsync()
    {
        var ct = _cts.Token;
        try
        {
            string wsUrl = "";
            using (var http = new HttpClient { Timeout = TimeSpan.FromSeconds(2) })
            {
                for (var i = 0; i < 50 && wsUrl.Length == 0 && !ct.IsCancellationRequested; i++)
                {
                    try
                    {
                        var json = await http.GetStringAsync($"http://127.0.0.1:{_port}/json/version", ct).ConfigureAwait(false);
                        wsUrl = JsonNode.Parse(json)?["webSocketDebuggerUrl"]?.GetValue<string>() ?? "";
                    }
                    catch (Exception e) when (e is HttpRequestException or TaskCanceledException or System.Text.Json.JsonException) { }
                    if (wsUrl.Length == 0) await Task.Delay(400, ct).ConfigureAwait(false);
                }
            }
            if (wsUrl.Length == 0)
            {
                Status?.Invoke("連不上瀏覽器的偵錯埠。請關閉所有用同一個設定檔開啟的瀏覽器視窗後再試。");
                return;
            }
            _ws = new ClientWebSocket();
            await _ws.ConnectAsync(new Uri(wsUrl), ct).ConfigureAwait(false);
            Status?.Invoke("已連上瀏覽器：請在瀏覽器裡開啟並播放影片");
            await SendAsync(NewId(), "Target.setAutoAttach", new JsonObject { ["autoAttach"] = true, ["waitForDebuggerOnStart"] = false, ["flatten"] = true }, null);
            _getTargetsId = NewId();
            await SendAsync(_getTargetsId, "Target.getTargets", null, null);

            var buffer = new byte[1 << 16];
            using var message = new MemoryStream();
            while (!ct.IsCancellationRequested && _ws.State == WebSocketState.Open)
            {
                var r = await _ws.ReceiveAsync(buffer, ct).ConfigureAwait(false);
                if (r.MessageType == WebSocketMessageType.Close) break;
                message.Write(buffer, 0, r.Count);
                if (!r.EndOfMessage) continue;
                var raw = Encoding.UTF8.GetString(message.GetBuffer(), 0, (int)message.Length);
                message.SetLength(0);
                await OnMessageAsync(raw).ConfigureAwait(false);
            }
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or IOException or ObjectDisposedException) { }
        finally
        {
            if (!_cts.IsCancellationRequested) Status?.Invoke("瀏覽器已關閉，擷取結束");
        }
    }

    private int NewId() => Interlocked.Increment(ref _nextId);

    private async Task SendAsync(int id, string method, JsonObject? parameters, string? sessionId)
    {
        var msg = new JsonObject { ["id"] = id, ["method"] = method, ["params"] = parameters ?? new JsonObject() };
        if (sessionId is not null) msg["sessionId"] = sessionId;
        var bytes = Encoding.UTF8.GetBytes(msg.ToJsonString());
        await _sendLock.WaitAsync(_cts.Token).ConfigureAwait(false);
        try
        {
            if (_ws is { State: WebSocketState.Open })
                await _ws.SendAsync(bytes, WebSocketMessageType.Text, true, _cts.Token).ConfigureAwait(false);
        }
        finally { _sendLock.Release(); }
    }

    private async Task OnMessageAsync(string raw)
    {
        // 播放影片時瀏覽器會每秒送上千則 dataReceived 之類的事件；不是要用的就連 JSON 都不解析
        if (!(raw.Contains("\"id\":") || raw.Contains("Network.responseReceived\"") || raw.Contains("Target.attachedToTarget"))) return;
        if (JsonNode.Parse(raw) is not JsonObject m) return;
        var rid = m["id"]?.GetValue<int>();
        if (rid is not null && rid == _getTargetsId && _getTargetsId != 0)
        {
            foreach (var t in m["result"]?["targetInfos"]?.AsArray() ?? [])
                if (t?["type"]?.GetValue<string>() == "page")
                    await SendAsync(NewId(), "Target.attachToTarget", new JsonObject { ["targetId"] = t["targetId"]!.GetValue<string>(), ["flatten"] = true }, null);
            return;
        }
        if (rid is int id)
        {
            bool known;
            lock (_gate) known = _pendingEval.Contains(id);
            if (known)                                                      // Runtime.evaluate 的結果
            {
                var value = m["result"]?["result"]?["value"]?.GetValue<string>() ?? "[]";
                lock (_gate)
                {
                    try
                    {
                        foreach (var row in JsonNode.Parse(value)?.AsArray() ?? [])
                            if (row is JsonArray a && a.Count >= 5) _videos.Add(a.Select(x => x?.GetValue<double>() ?? -1).ToArray());
                    }
                    catch (Exception e) when (e is System.Text.Json.JsonException or InvalidOperationException or FormatException) { }
                    _pendingEval.Remove(id);
                    if (_pendingEval.Count == 0) _evalDone?.TrySetResult(true);
                }
                return;
            }
        }
        var method = m["method"]?.GetValue<string>() ?? "";
        var p = m["params"];
        if (method == "Target.attachedToTarget")
        {
            var sid = p?["sessionId"]?.GetValue<string>() ?? "";
            var type = p?["targetInfo"]?["type"]?.GetValue<string>() ?? "";
            lock (_gate) _sessions[sid] = type;
            await SendAsync(NewId(), "Network.enable", null, sid);
            // 網頁裡的 worker／iframe 是另外的目標，Meta 的影片就是 worker 去抓的；每一層都要接著監聽
            await SendAsync(NewId(), "Target.setAutoAttach", new JsonObject { ["autoAttach"] = true, ["waitForDebuggerOnStart"] = false, ["flatten"] = true }, sid);
            if (!_navigated && _startUrl.Length > 0 && type == "page")
            {
                _navigated = true;       // 連上之後才導向，避免頁面在開始監聽前就載完而漏掉請求
                await SendAsync(NewId(), "Page.navigate", new JsonObject { ["url"] = _startUrl }, sid);
            }
        }
        else if (method == "Network.responseReceived")
        {
            var resp = p?["response"];
            var url = resp?["url"]?.GetValue<string>() ?? "";
            var mime = resp?["mimeType"]?.GetValue<string>() ?? "";
            var kind = CaptureUtil.CandidateKind(url, mime);
            if (kind.Length == 0 || (resp?["status"]?.GetValue<int>() ?? 0) >= 400) return;
            var c = new Candidate { Url = CaptureUtil.CleanMediaUrl(url), Mime = mime, Kind = kind, Referer = CaptureUtil.Header(resp?["requestHeaders"], "Referer") };
            var range = CaptureUtil.Header(resp?["headers"], "Content-Range");          // 範圍請求的 Content-Range 末尾是整個檔案的大小
            var size = range.Contains('/') ? range[(range.LastIndexOf('/') + 1)..] : CaptureUtil.Header(resp?["headers"], "Content-Length");
            c.Size = long.TryParse(size, NumberStyles.None, CultureInfo.InvariantCulture, out var n) && n > 0 ? n : -1;
            Candidate? announce = null;
            lock (_gate)
            {
                if (!_seen.TryGetValue(c.Url, out var old))
                {
                    c.Seq = _seen.Count;
                    _seen[c.Url] = c;
                    announce = c;
                }
                else if (c.Size > old.Size || (old.Referer.Length == 0 && c.Referer.Length > 0))
                {
                    old.Size = Math.Max(old.Size, c.Size);
                    if (old.Referer.Length == 0) old.Referer = c.Referer;
                    announce = old;
                }
            }
            if (announce is not null) Found?.Invoke(announce);
        }
    }

    /// <summary>
    /// 問頁面裡正在播放的 &lt;video&gt;：長度幾秒。正在播放的那支：沒暫停、已經播了一點的；沒有的話取播放進度最多的。
    /// 長度不明（直播等）或讀不到回傳 -1。
    /// </summary>
    public async Task<double> QueryPlayingVideoAsync(int timeoutMs = 3000)
    {
        const string expression =
            "JSON.stringify([...document.querySelectorAll('video')].map(v=>[Number.isFinite(v.duration)?v.duration:-1,v.paused?1:0,v.currentTime,v.videoWidth,v.videoHeight]))";
        string[] targets;
        lock (_gate)
        {
            _videos.Clear();
            _pendingEval.Clear();
            _evalDone = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            targets = [.. _sessions.Where(kv => kv.Value is "page" or "iframe").Select(kv => kv.Key)];
            foreach (var _ in targets) _pendingEval.Add(0);       // 先佔位，下面換成真正的 id
            _pendingEval.Clear();
        }
        if (targets.Length == 0) return -1;
        var ids = targets.Select(_ => NewId()).ToArray();
        lock (_gate) foreach (var id in ids) _pendingEval.Add(id);
        try
        {
            for (var i = 0; i < targets.Length; i++)
                await SendAsync(ids[i], "Runtime.evaluate", new JsonObject { ["expression"] = expression, ["returnByValue"] = true }, targets[i]);
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException) { return -1; }
        await Task.WhenAny(_evalDone!.Task, Task.Delay(timeoutMs)).ConfigureAwait(false);        // 有頁面沒回應也不能一直等
        double duration = -1, best = -1;
        var playingFound = false;
        lock (_gate)
            foreach (var v in _videos)
            {
                var playing = v[1] == 0 && v[2] > 0.3;
                var score = (playing ? 1e6 : 0) + v[2];
                if (v[0] > 0 && score > best) { best = score; duration = v[0]; playingFound = playing; }
            }
        LastQueryPlaying = playingFound;
        return duration;
    }

    /// <summary>關掉整個瀏覽器（自動擷取成功後用；獨立設定檔的登入資訊會留著）。</summary>
    public async Task CloseBrowserAsync()
    {
        try
        {
            await SendAsync(NewId(), "Browser.close", new JsonObject(), null).ConfigureAwait(false);
            await Task.Delay(300).ConfigureAwait(false);
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException or ObjectDisposedException or InvalidOperationException) { }
        Stop();
    }
}

/// <summary>偵測到的影音 → 挑出要下載的 → 下載 → 挑最好的影像與聲音 → 合併。</summary>
public static class CaptureRunner
{
    private sealed record FileInfoEx(string Path, bool Video, bool Audio, int W, int H, double Dur, long Size)
    {
        public double Pixels => (double)W * H;
        public double Bitrate => Dur > 0 ? Size * 8.0 / Dur : 0;
    }

    private const int ParallelDownloads = 4;   // 同時下載幾個（太多會被 CDN 當成異常連線）

    private static readonly Regex Percent = new(@"\[download\]\s+([\d.]+)%", RegexOptions.Compiled);

    /// <summary>有串流（m3u8／mpd）就用串流（yt-dlp 自己會挑最高的）；否則把偵測到的檔案（最多 12 個）全部抓下來，網址已經帶長度的先把明顯是別支影片的略過。</summary>
    public static List<Candidate> PickCandidates(IEnumerable<Candidate> candidates, double targetDuration)
    {
        var ordered = candidates.OrderBy(c => c.Seq).ToList();
        if (ordered.FirstOrDefault(c => c.Kind == "stream") is { } stream) return [stream];
        if (targetDuration > 0)
        {
            var same = ordered.Where(c => CaptureUtil.UrlDuration(c.Url) is var d && (d < 0 || Math.Abs(d - targetDuration) <= 1.5)).ToList();
            if (same.Count > 0) ordered = same;
        }
        return ordered.Take(CaptureUtil.MaxFiles).ToList();
    }

    public static async Task<string?> DownloadAndMergeAsync(
        IEnumerable<Candidate> candidates, double targetDuration, string outDir, Engine engine, string? ffmpegDir,
        Action<string> log, Action<double>? progress = null, Action<string>? status = null, CancellationToken ct = default)
    {
        var queue = PickCandidates(candidates, targetDuration);
        if (queue.Count == 0) return null;
        Directory.CreateDirectory(outDir);
        var stamp = DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
        var results = new string?[queue.Count];            // 依偵測順序存放，合併時的挑選順序不受完成先後影響
        var percents = new double[queue.Count];
        var noImpersonate = false;                         // 某一項發現這個 yt-dlp 不能偽裝成瀏覽器後，其餘項目都不帶 --impersonate
        var doneCount = 0;
        status?.Invoke($"同時下載 {queue.Count} 項（最多 {ParallelDownloads} 個一起）…");
        using var gate = new SemaphoreSlim(ParallelDownloads);

        async Task OneAsync(int i)
        {
            await gate.WaitAsync(ct).ConfigureAwait(false);
            try
            {
                if (ct.IsCancellationRequested) return;
                var c = queue[i];
                log($"第 {i + 1}/{queue.Count} 項開始：[{c.Kind}] {(c.Size < 0 ? "大小未知" : (c.Size / 1048576.0).ToString("F1", CultureInfo.InvariantCulture) + " MB")} {(c.Url.Length > 110 ? c.Url[..110] : c.Url)}");
                var clock = Stopwatch.StartNew();
                void ItemProgress(double pct)
                {
                    if (progress is null) return;
                    lock (percents)
                    {
                        percents[i] = pct;
                        progress(percents.Sum() / percents.Length);
                    }
                }
                string? path;
                while (true)
                {
                    var (code, file) = await DownloadOneAsync(c, Path.Combine(outDir, $"擷取_{stamp}_{i + 1}.%(ext)s"), engine, ffmpegDir, !Volatile.Read(ref noImpersonate), ItemProgress, ct).ConfigureAwait(false);
                    if (ct.IsCancellationRequested) return;
                    // fbcdn 這類 CDN 對 yt-dlp 預設的連線特徵會限速（實測同一個 0.6 MB 檔案：預設 86 秒，偽裝成 Chrome 只要 2.5 秒），
                    // 所以先以 Chrome 身分連線；沒裝 curl_cffi 時 yt-dlp 會立刻報錯，那一項改用預設方式重試。
                    if (code != 0 && !Volatile.Read(ref noImpersonate) && clock.ElapsedMilliseconds < 15000)
                    {
                        Volatile.Write(ref noImpersonate, true);
                        log($"以 Chrome 身分連線失敗，改用預設方式重試第 {i + 1} 項…");
                        continue;
                    }
                    path = code == 0 ? file : null;
                    break;
                }
                results[i] = path;
                lock (percents) percents[i] = 100;
                log(path is not null ? $"第 {i + 1} 項完成（{clock.ElapsedMilliseconds / 1000} 秒）" : $"第 {i + 1} 項下載失敗");
                status?.Invoke($"下載中：已完成 {Interlocked.Increment(ref doneCount)}/{queue.Count} 項…");
            }
            finally { gate.Release(); }
        }

        try { await Task.WhenAll(Enumerable.Range(0, queue.Count).Select(OneAsync)).ConfigureAwait(false); }
        catch (OperationCanceledException) { return null; }
        if (ct.IsCancellationRequested) return null;
        var files = results.Where(r => r is not null).Select(r => r!).ToList();
        status?.Invoke("挑選最高畫質與音質，合併中…");
        return await ChooseAndMergeAsync(files, ffmpegDir, targetDuration, outDir, stamp, log).ConfigureAwait(false);
    }

    private static async Task<(int Code, string? File)> DownloadOneAsync(
        Candidate c, string template, Engine engine, string? ffmpegDir, bool impersonate, Action<double>? progress, CancellationToken ct)
    {
        var a = new List<string>
        {
            "--ignore-config", "--newline", "--color", "never", "--no-playlist", "-q", "--progress", "--no-simulate",
            "--retries", "10", "--fragment-retries", "10", "--socket-timeout", "30", "--windows-filenames",
            "--merge-output-format", "mp4/mkv", "-o", template, "--print", "after_move:AVD-FILE|%(filepath)s",
        };
        if (!string.IsNullOrEmpty(ffmpegDir)) a.AddRange(["--ffmpeg-location", ffmpegDir]);
        if (c.Referer.Length > 0) a.AddRange(["--add-header", "Referer:" + c.Referer]);
        if (impersonate) a.AddRange(["--impersonate", "chrome"]);
        a.AddRange(["--", c.Url]);

        using var p = Core.CreateProcess(engine.Program, engine.Args(a));
        try { p.Start(); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { return (-1, null); }
        string? file = null;
        var lastOutput = Stopwatch.StartNew();
        async Task Pump(StreamReader reader)
        {
            while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
            {
                lastOutput.Restart();
                if (line.StartsWith("AVD-FILE|", StringComparison.Ordinal)) file = line[9..].Trim();
                else if (Percent.Match(line) is { Success: true } m && double.TryParse(m.Groups[1].Value, NumberStyles.Float, CultureInfo.InvariantCulture, out var pct)) progress?.Invoke(pct);
            }
        }
        using var stop = CancellationTokenSource.CreateLinkedTokenSource(ct);
        // 一段時間沒有任何輸出就當作卡住，結束這一項
        var watchdog = Task.Run(async () =>
        {
            while (!stop.IsCancellationRequested)
            {
                try { await Task.Delay(1000, stop.Token).ConfigureAwait(false); } catch (OperationCanceledException) { return; }
                if (lastOutput.Elapsed > TimeSpan.FromSeconds(60)) { Core.KillTree(p); return; }
            }
        });
        using (ct.Register(() => Core.KillTree(p)))
        {
            await Task.WhenAll(Pump(p.StandardOutput), Pump(p.StandardError)).ConfigureAwait(false);
            await p.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
        }
        stop.Cancel();
        await watchdog.ConfigureAwait(false);
        return (p.ExitCode, file);
    }

    private static async Task<FileInfoEx> ProbeAsync(string path, string? ffmpegDir)
    {
        var size = new FileInfo(path).Length;
        if (string.IsNullOrEmpty(ffmpegDir)) return new(path, true, false, 0, 0, -1, size);
        using var p = Core.CreateProcess(Path.Combine(ffmpegDir, "ffprobe.exe"),
            ["-v", "error", "-print_format", "json", "-show_entries", "stream=codec_type,width,height:format=duration", path]);
        try { p.Start(); }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { return new(path, true, false, 0, 0, -1, size); }
        var stdout = p.StandardOutput.ReadToEndAsync();
        _ = p.StandardError.ReadToEndAsync();
        await p.WaitForExitAsync().ConfigureAwait(false);
        bool video = false, audio = false;
        int w = 0, h = 0;
        double dur = -1;
        try
        {
            var o = JsonNode.Parse(await stdout.ConfigureAwait(false));
            foreach (var s in o?["streams"]?.AsArray() ?? [])
            {
                var type = s?["codec_type"]?.GetValue<string>();
                if (type == "video" && !video) { video = true; w = s?["width"]?.GetValue<int>() ?? 0; h = s?["height"]?.GetValue<int>() ?? 0; }
                else if (type == "audio") audio = true;
            }
            if (double.TryParse(o?["format"]?["duration"]?.GetValue<string>(), NumberStyles.Float, CultureInfo.InvariantCulture, out var d)) dur = d;
        }
        catch (Exception e) when (e is System.Text.Json.JsonException or InvalidOperationException or FormatException) { }
        return new(path, video, audio, w, h, dur, size);
    }

    /// <summary>下載到的檔案裡挑出「正在播放的那支」最好的影像與聲音，合併成 mp4。回傳結果路徑。</summary>
    public static async Task<string?> ChooseAndMergeAsync(
        List<string> files, string? ffmpegDir, double targetDuration, string outDir, string stamp, Action<string> log)
    {
        files = files.Where(File.Exists).ToList();
        if (files.Count == 0)
        {
            log("沒有下載到任何檔案，原因見上方紀錄。");
            return null;
        }
        var infos = new List<FileInfoEx>();
        foreach (var f in files) infos.Add(await ProbeAsync(f, ffmpegDir).ConfigureAwait(false));
        foreach (var i in infos)
            log($"  {Path.GetFileName(i.Path)}：{(i.Video ? $"影像 {i.W}×{i.H} " : "")}{(i.Audio ? "聲音" : "")} {i.Dur.ToString("F2", CultureInfo.InvariantCulture)} 秒 {i.Size / 1024} KB");

        var pool = infos;
        if (targetDuration > 0)
        {
            // 只留「正在播放的那支」：長度和頁面上的 <video> 最接近的那一組。同一支影片的各種畫質、影像與聲音，長度只差幾百分之一秒；
            // 預先載入的下一支影片長度不同，即使只差 0.4 秒也不能混進來。所以先找出最接近的差距，只留和它差不到 0.15 秒的。
            var diffs = infos.Where(i => i.Dur > 0).Select(i => Math.Abs(i.Dur - targetDuration)).ToList();
            var bestDiff = diffs.Count > 0 ? diffs.Min() : 1e9;
            var same = bestDiff <= Math.Max(0.6, targetDuration * 0.05)
                ? infos.Where(i => i.Dur > 0 && Math.Abs(i.Dur - targetDuration) <= bestDiff + 0.15).ToList()
                : [];
            if (same.Count > 0) pool = same;
            log($"長度最接近正在播放影片的檔案：{same.Count}／{infos.Count} 個");
        }
        // 最高畫質＝解析度最大、同解析度取位元率最高；最高音質＝位元率最高。
        static bool Better(FileInfoEx a, FileInfoEx b) => a.Pixels != b.Pixels ? a.Pixels > b.Pixels : a.Bitrate > b.Bitrate;
        FileInfoEx? whole = null, bestV = null, bestA = null;
        foreach (var i in pool)
        {
            if (i.Video && i.Audio) { if (whole is null || Better(i, whole)) whole = i; }
            else if (i.Video) { if (bestV is null || Better(i, bestV)) bestV = i; }
            else if (i.Audio) { if (bestA is null || i.Bitrate > bestA.Bitrate) bestA = i; }
        }
        // 影像與聲音分開的組合，和影像聲音在一起的檔案比：解析度較高（或一樣高）的組合勝出，因為聲音也是挑最高的。
        var pair = bestV is not null && bestA is not null && (whole is null || bestV.Pixels >= whole.Pixels);
        static void Remove(string f) { try { File.Delete(f); } catch (IOException) { } catch (UnauthorizedAccessException) { } }
        if (!pair)
        {
            string keep;
            if (whole is not null) { log($"選用：影像聲音在一起的檔案 {whole.W}×{whole.H}"); keep = whole.Path; }
            else if (bestV is not null) { log($"只找到影像（{bestV.W}×{bestV.H}），沒有找到聲音"); keep = bestV.Path; }
            else keep = files[0];
            foreach (var f in files.Where(f => f != keep)) Remove(f);
            log("完成：" + keep);
            return keep;
        }
        log($"選用：影像 {bestV!.W}×{bestV.H}、聲音 {(long)(bestA!.Bitrate / 1000)} kbps");
        if (string.IsNullOrEmpty(ffmpegDir))
        {
            log("找不到 ffmpeg，無法合併影像與聲音；檔案留在儲存位置。");
            return bestV.Path;
        }
        var outPath = Path.Combine(outDir, $"擷取_{stamp}.mp4");
        using var ff = Core.CreateProcess(Path.Combine(ffmpegDir, "ffmpeg.exe"),
            ["-y", "-loglevel", "error", "-i", bestV.Path, "-i", bestA.Path, "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", "-shortest", "-movflags", "+faststart", outPath]);
        ff.Start();
        var err = ff.StandardError.ReadToEndAsync();
        _ = ff.StandardOutput.ReadToEndAsync();
        await ff.WaitForExitAsync().ConfigureAwait(false);
        if (ff.ExitCode != 0 || !File.Exists(outPath))
        {
            var text = (await err.ConfigureAwait(false)).Trim();
            log("合併失敗：" + (text.Length > 300 ? text[^300..] : text));
            return bestV.Path;
        }
        foreach (var f in files) Remove(f);
        log("完成：" + outPath);
        return outPath;
    }

    private const int AutoWaitSeconds = 30;      // 自動擷取：等到「有影音、而且有一支正在播放」的時間上限；超過就交給使用者手動操作（多半要登入）
    private const int AutoSettleMs = 2000;       // 偵測到的影音檔案這麼久沒再增加，就當作影像與聲音都到齊了

    /// <summary>
    /// 自動擷取（瀏覽器已開、網址已導向）：偵測到正在播放的影片就立刻鎖定它的長度，等影音到齊後下載，再用長度比對下載結果。
    /// 原因："ok" 成功；"manual" 一直沒有正在播放的影片（多半要登入，交給使用者）；"mismatch" 下載了但長度不符；"failed" 沒下載到；"cancelled" 使用者停止。
    /// </summary>
    public static async Task<(string? Path, string Reason, double Target)> AutoCaptureAsync(
        CaptureSession session, string outDir, Engine engine, string? ffmpegDir,
        Action<string> log, Action<double>? progress = null, Action<string>? status = null, CancellationToken ct = default)
    {
        status?.Invoke("等待網頁開始播放影片…");
        var deadline = DateTime.UtcNow.AddSeconds(AutoWaitSeconds);
        double target = -1;
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            if (session.Candidates.Count > 0)
            {
                var d = await session.QueryPlayingVideoAsync(2000).ConfigureAwait(false);
                if (d > 0 && session.LastQueryPlaying) { target = d; break; }
            }
            await Task.Delay(500, CancellationToken.None).ConfigureAwait(false);
        }
        if (ct.IsCancellationRequested) return (null, "cancelled", target);
        if (target <= 0) return (null, "manual", target);
        log($"鎖定正在播放的影片長度：{target.ToString("F2", CultureInfo.InvariantCulture)} 秒");
        status?.Invoke("等待影像與聲音都出現…");
        var count = session.Candidates.Count;
        var changed = Stopwatch.StartNew();
        var settle = Stopwatch.StartNew();
        while (settle.ElapsedMilliseconds < 8000 && !ct.IsCancellationRequested)
        {
            if (session.Candidates.Count != count) { count = session.Candidates.Count; changed.Restart(); }
            else if (changed.ElapsedMilliseconds >= AutoSettleMs) break;
            await Task.Delay(200, CancellationToken.None).ConfigureAwait(false);
        }
        if (ct.IsCancellationRequested) return (null, "cancelled", target);
        var path = await DownloadAndMergeAsync(session.Candidates, target, outDir, engine, ffmpegDir, log, progress, status, ct).ConfigureAwait(false);
        if (ct.IsCancellationRequested) return (null, "cancelled", target);
        if (path is null) return (null, "failed", target);
        var got = (await ProbeAsync(path, ffmpegDir).ConfigureAwait(false)).Dur;
        if (got > 0 && Math.Abs(got - target) <= Math.Max(0.6, target * 0.05))
        {
            log($"長度比對成功：下載 {got.ToString("F2", CultureInfo.InvariantCulture)} 秒，正在播放 {target.ToString("F2", CultureInfo.InvariantCulture)} 秒");
            return (path, "ok", target);
        }
        log($"長度比對不符：下載 {got.ToString("F2", CultureInfo.InvariantCulture)} 秒，正在播放 {target.ToString("F2", CultureInfo.InvariantCulture)} 秒");
        return (path, "mismatch", target);
    }

    /// <summary>不開介面：開瀏覽器載入網址、等影音出現、再等幾秒讓影像與聲音都出現、下載並合併。回傳結果路徑。給測試用。</summary>
    public static async Task<string?> RunHeadlessAsync(string url, string outDir, double waitSeconds, Engine engine, string? ffmpegDir, Action<string> log)
    {
        using var session = new CaptureSession();
        session.Found += c => log($"偵測到 [{c.Kind}] {c.Mime} {(c.Url.Length > 90 ? c.Url[..90] : c.Url)}");
        session.Status += log;
        try { session.Start(url); }
        catch (InvalidOperationException e) { log(e.Message); return null; }
        var deadline = DateTime.UtcNow.AddSeconds(45);
        while (session.Candidates.Count == 0 && DateTime.UtcNow < deadline) await Task.Delay(200).ConfigureAwait(false);
        if (session.Candidates.Count == 0) { log("沒有偵測到任何影音。"); return null; }
        await Task.Delay(TimeSpan.FromSeconds(waitSeconds)).ConfigureAwait(false);
        var target = await session.QueryPlayingVideoAsync().ConfigureAwait(false);
        log(target > 0 ? $"正在播放的影片長度：{target.ToString("F1", CultureInfo.InvariantCulture)} 秒" : "讀不到正在播放的影片長度，改用偵測順序判斷");
        return await DownloadAndMergeAsync(session.Candidates, target, outDir, engine, ffmpegDir, log).ConfigureAwait(false);
    }
}
