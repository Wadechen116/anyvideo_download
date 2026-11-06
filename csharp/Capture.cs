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

/// <summary>頁面上目前這一篇抖音圖文貼文：圖片網址（依頁面順序，高解析版、簽章完整）與背景音樂網址（可能是空字串）。</summary>
public sealed record PostInfo(List<string> Slides, string Music);

public sealed class Candidate
{
    public string Url { get; set; } = "";       // 已去掉 bytestart／byteend（Meta 把一個檔切成很多段請求）
    public string Mime { get; set; } = "";
    public string Referer { get; set; } = "";
    public string Kind { get; set; } = "";      // stream（m3u8／mpd）、video、audio、media、slide（抖音圖文的圖片）
    public long Size { get; set; } = -1;        // 未知為 -1
    public int Seq { get; set; }                // 第幾個被偵測到（越小越早）
}

/// <summary>抖音頁面自己抓的資料（/aweme/v1/web/… 回應裡的 aweme 物件）中的一篇貼文。用影片 ID 對應出「屬於這篇的檔案」，
/// 不靠長度猜（不同影片長度可能一樣）。</summary>
public sealed class DouyinPost
{
    public string Id { get; init; } = "";
    public double Duration { get; set; } = -1;                 // 秒；不知道是 -1
    public HashSet<string> FileKeys { get; } = [];             // 這篇所有畫質的影片檔身分（CaptureUtil.DouyinFileKey）
    public Candidate? Best { get; set; }                       // 最高畫質的影片檔（解析度最大、同解析度位元率最高）
    public int Width { get; set; }
    public int Height { get; set; }
    public double Bitrate { get; set; }
    public PostInfo? Post { get; set; }                        // 圖文貼文：圖片（依順序）與音樂；不是圖文為 null
}

public static partial class CaptureUtil
{
    public const int MaxFiles = 12;             // 最多下載幾個偵測到的檔案

    /// <summary>抖音影片檔的身分：網址路徑裡 /video/tos/ 之後的部分（同一個檔案在不同 CDN 主機、不同簽章下都一樣）。不是抖音影片檔回傳空字串。</summary>
    public static string DouyinFileKey(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u)) return "";
        var m = DouyinFileKeyRe().Match(u.AbsolutePath);
        return m.Success ? m.Groups[1].Value : "";
    }

    [GeneratedRegex("/video/tos/(.+?)/?$")]
    private static partial Regex DouyinFileKeyRe();

    private static string Str(JsonNode? n) { try { return n is JsonValue v && v.TryGetValue<string>(out var s) ? s : ""; } catch (InvalidOperationException) { return ""; } }
    private static double Num(JsonNode? n) { try { return n is JsonValue v && v.TryGetValue<double>(out var d) ? d : -1; } catch (InvalidOperationException) { return -1; } }

    /// <summary>抖音的 aweme 物件 → DouyinPost。影片：所有畫質的檔案身分、最高畫質那個檔案；圖文：圖片（依順序）與音樂。不像貼文回傳 null。</summary>
    public static DouyinPost? ParseAweme(JsonObject o)
    {
        var id = Str(o["aweme_id"]);                         // 數字型別的 ID 可能失去精度，不採用
        if (id.Length == 0) return null;
        var p = new DouyinPost { Id = id };
        var video = o["video"] as JsonObject;
        var ms = Num(video?["duration"]);
        if (ms <= 0) ms = Num(o["duration"]);
        p.Duration = ms > 0 ? ms / 1000.0 : -1;
        void Walk(JsonNode? n, int depth)                    // 影片底下所有網址（各畫質、h264／h265、下載用）都算這篇的檔案
        {
            if (n is null || depth > 8) return;
            if (n is JsonArray a) { foreach (var x in a) Walk(x, depth + 1); }
            else if (n is JsonObject ob) { foreach (var kv in ob) Walk(kv.Value, depth + 1); }
            else if (DouyinFileKey(Str(n)) is { Length: > 0 } k) p.FileKeys.Add(k);
        }
        Walk(video, 0);
        void Consider(JsonObject? addr, double bitrate)      // 最高畫質：解析度最大，同解析度取位元率最高
        {
            if (addr is null) return;
            var url = (addr["url_list"] as JsonArray ?? []).Select(Str).FirstOrDefault(u => u.StartsWith("https://", StringComparison.Ordinal) && DouyinFileKey(u).Length > 0);
            if (url is null) return;
            var w = (int)Math.Max(0, Num(addr["width"])); var h = (int)Math.Max(0, Num(addr["height"]));
            double px = (double)w * h, bestPx = (double)p.Width * p.Height;
            if (p.Best is not null && (px < bestPx || (px == bestPx && bitrate <= p.Bitrate))) return;
            var bytes = Num(addr["data_size"]);
            p.Best = new Candidate { Url = url, Mime = "video/mp4", Referer = "https://www.douyin.com/", Kind = "video", Size = bytes > 0 ? (long)bytes : -1 };
            p.Width = w; p.Height = h; p.Bitrate = Math.Max(0, bitrate);
        }
        foreach (var b in video?["bit_rate"] as JsonArray ?? [])
            if (b is JsonObject bo) Consider(bo["play_addr"] as JsonObject, Num(bo["bit_rate"]));
        if (p.Best is null) Consider(video?["play_addr"] as JsonObject, 0);
        var slides = new List<string>();
        foreach (var img in o["images"] as JsonArray ?? [])
            if ((img?["url_list"] as JsonArray ?? []).Select(Str).FirstOrDefault(u => u.StartsWith("http", StringComparison.Ordinal)) is { } src) slides.Add(src);
        if (slides.Count > 0)
            p.Post = new PostInfo(slides, (o["music"]?["play_url"]?["url_list"] as JsonArray ?? []).Select(Str).FirstOrDefault(u => u.StartsWith("http", StringComparison.Ordinal)) ?? "");
        return p.FileKeys.Count > 0 || p.Post is not null ? p : null;
    }

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
        if (m.StartsWith("image/") && url.Contains("aweme-images")) return "slide";   // 抖音圖文貼文的圖片（封面、頭像不帶這個字）
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
    private int _queryGen;                  // 查詢世代：查詢結束時封存，遲到的回覆（已不在 _pendingEval）不會再改結果
    private string _qAct = "", _qHref = "";  // 進行中那次查詢：主頁面回報的目前那篇 ID 與網址
    private int _nextId;
    private int _getTargetsId;
    private int _port;
    private string _startUrl = "";
    private bool _navigated;
    private readonly Dictionary<string, DouyinPost> _posts = [];       // 抖音影片 ID -> 貼文資料
    private readonly Dictionary<string, string> _jsonReqs = [];        // 等下載完的抖音資料請求：requestId -> sessionId
    private readonly HashSet<string> _jsonSeen = [];                   // 已處理過的 requestId（同一頁可能連上兩次，事件會重複）
    private readonly Dictionary<int, string> _bodyReqs = [];           // Network.getResponseBody 的指令 id -> requestId

    public event Action<Candidate>? Found;
    public event Action<string>? Status;

    public string Profile { get; }
    /// <summary>上一次 QueryPlayingVideoAsync 讀到的那支是不是真的在播（沒暫停、播了一點）。</summary>
    public bool LastQueryPlaying { get; private set; }

    /// <summary>上一次 QueryPlayingVideoAsync 找到指定抖音影片 ID 那支的長度（不一定還在播：可能已播完換下一支）。</summary>
    public bool LastQueryTargetHit { get; private set; }

    /// <summary>開啟網址裡的抖音影片 ID（自動擷取只認這支）；沒有是空字串。帳號開著自動連播時，短影片播完頁面就換下一支，
    /// 網址的 modal_id 與 feed-active-video 都會跟著變，所以開始時就固定下來。</summary>
    public string StartTargetId { get; private set; } = "";

    /// <summary>開啟的是抖音網址（含沒有 ID 的短連結：轉址後第一次出現 ID 就固定成 StartTargetId）。</summary>
    public bool StartIsDouyin { get; private set; }

    /// <summary>上一次 QueryPlayingVideoAsync 時頁面目前顯示那篇的抖音 ID（feed-active-video）；查不到是空字串。手動下載用。</summary>
    public string LastActiveId { get; private set; } = "";

    /// <summary>這個抖音影片 ID 的貼文資料；沒有回傳 null。</summary>
    public DouyinPost? GetDouyinPost(string id) { if (id.Length == 0) return null; lock (_gate) return _posts.GetValueOrDefault(id); }

    private void CollectAwemes(JsonNode? n, int depth)
    {
        if (n is null || depth > 12) return;
        if (n is JsonArray a) { foreach (var x in a) CollectAwemes(x, depth + 1); return; }
        if (n is not JsonObject o) return;
        if (o.ContainsKey("aweme_id") && (o.ContainsKey("video") || o.ContainsKey("images")))
        {
            if (CaptureUtil.ParseAweme(o) is not { } p) return;
            lock (_gate)
            {
                if (_posts.TryGetValue(p.Id, out var old))
                {
                    // 同一篇在不同回應出現：檔案身分合併；最高畫質取兩邊較好的（同畫質用新的網址）；缺的欄位保留舊值
                    p.FileKeys.UnionWith(old.FileKeys);
                    double np = (double)p.Width * p.Height, op = (double)old.Width * old.Height;
                    if (old.Best is not null && (p.Best is null || np < op || (np == op && p.Bitrate < old.Bitrate)))
                    { p.Best = old.Best; p.Width = old.Width; p.Height = old.Height; p.Bitrate = old.Bitrate; }
                    if (p.Duration <= 0) p.Duration = old.Duration;
                    p.Post ??= old.Post;
                }
                else if (_posts.Count >= 5000) return;          // 上限：一頁不會有這麼多篇
                _posts[p.Id] = p;
            }
            return;
        }
        foreach (var kv in o)
            if (kv.Value is JsonObject or JsonArray) CollectAwemes(kv.Value, depth + 1);
    }

    /// <summary>上一次 QueryPlayingVideoAsync 讀到的抖音圖文貼文（頁面上「目前這一篇」的圖片依頁面順序、背景音樂網址）；不是圖文為 null。</summary>
    public PostInfo? LastPost { get; private set; }
    private PostInfo? _post;

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
        StartTargetId = Platforms.DouyinVideoId(startUrl);
        StartIsDouyin = Uri.TryCreate(startUrl.Trim(), UriKind.Absolute, out var su) &&
                        (su.Host.ToLowerInvariant() is var sh && (sh is "douyin.com" or "iesdouyin.com" || sh.EndsWith(".douyin.com", StringComparison.Ordinal) || sh.EndsWith(".iesdouyin.com", StringComparison.Ordinal)));
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
        bool waitingJson;
        lock (_gate) waitingJson = _jsonReqs.Count > 0;
        if (!(raw.Contains("\"id\":") || raw.Contains("Network.responseReceived\"") || raw.Contains("Target.attachedToTarget") ||
              (waitingJson && (raw.Contains("Network.loadingFinished\"") || raw.Contains("Network.loadingFailed\""))))) return;
        if (JsonNode.Parse(raw) is not JsonObject m) return;
        var rid = m["id"]?.GetValue<int>();
        if (rid is not null && rid == _getTargetsId && _getTargetsId != 0)
        {
            foreach (var t in m["result"]?["targetInfos"]?.AsArray() ?? [])
                if (t?["type"]?.GetValue<string>() == "page")
                    await SendAsync(NewId(), "Target.attachToTarget", new JsonObject { ["targetId"] = t["targetId"]!.GetValue<string>(), ["flatten"] = true }, null);
            return;
        }
        if (rid is int bid)
        {
            bool isBody;
            lock (_gate) isBody = _bodyReqs.Remove(bid);
            if (isBody)                                                     // 抖音資料的內容
            {
                try
                {
                    var r = m["result"];
                    var body = r?["body"]?.GetValue<string>() ?? "";
                    // 讀不到或異常大（以位元組計，和另兩版一致）就略過：之後退回用長度判斷
                    var bytes = r?["base64Encoded"]?.GetValue<bool>() == true ? Convert.FromBase64String(body) : Encoding.UTF8.GetBytes(body);
                    if (bytes.Length <= 64 * 1024 * 1024) CollectAwemes(JsonNode.Parse(bytes), 0);
                }
                catch (Exception e) when (e is System.Text.Json.JsonException or InvalidOperationException or FormatException) { }
                return;
            }
        }
        if (rid is int id)
        {
            bool known;
            lock (_gate) known = _pendingEval.Contains(id);
            if (known)                                                      // Runtime.evaluate 的結果
            {
                var value = m["result"]?["result"]?["value"]?.GetValue<string>() ?? "{}";
                lock (_gate)
                {
                    if (!_pendingEval.Remove(id)) return;      // 查詢已封存（逾時結束或新查詢開始）：遲到的回覆不算
                    try
                    {
                        var reply = JsonNode.Parse(value);
                        foreach (var row in reply?["v"]?.AsArray() ?? [])
                            if (row is JsonArray a && a.Count >= 5) _videos.Add(a.Select(x => x?.GetValue<double>() ?? -1).ToArray());
                        // 頁面目前顯示的那篇：只採信主頁面（iframe 不是抖音的貼文頁）
                        var fromPage = _sessions.GetValueOrDefault(m["sessionId"]?.GetValue<string>() ?? "") == "page";
                        if (fromPage && _qAct.Length == 0 && reply?["act"]?.GetValue<string>() is { Length: > 0 } act) _qAct = act;
                        if (fromPage && _qHref.Length == 0 && reply?["href"]?.GetValue<string>() is { Length: > 0 } href) _qHref = href;
                        if ((_post is null || fromPage) && reply?["post"]?["slides"] is JsonArray ps && ps.Count > 0)
                            _post = new PostInfo(
                                [.. ps.Select(x => x?["src"]?.GetValue<string>() ?? "").Where(u => u != "")],
                                reply["post"]!["music"]?.GetValue<string>() ?? "");
                    }
                    catch (Exception e) when (e is System.Text.Json.JsonException or InvalidOperationException or FormatException) { }
                    if (_pendingEval.Count == 0) _evalDone?.TrySetResult(true);
                }
                return;
            }
        }
        var method = m["method"]?.GetValue<string>() ?? "";
        var p = m["params"];
        if (method is "Network.loadingFinished" or "Network.loadingFailed")
        {
            var req = p?["requestId"]?.GetValue<string>() ?? "";
            string? jsid;
            lock (_gate) { if (!_jsonReqs.Remove(req, out jsid)) return; }
            // 只讀可能含貼文清單的回應（貼文資料動輒數十 KB）；讀回應和查詢頁面都排在網頁主執行緒上，讀太多會拖慢查詢
            var len = p?["encodedDataLength"]?.GetValue<double>() ?? 0;
            if (method == "Network.loadingFinished" && len >= 4096 && len <= 32.0 * 1024 * 1024)
            {
                var bodyId = NewId();
                lock (_gate) _bodyReqs[bodyId] = req;
                await SendAsync(bodyId, "Network.getResponseBody", new JsonObject { ["requestId"] = req }, jsid);
            }
            return;
        }
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
            // 抖音頁面自己抓的貼文資料（推薦、作品列表、詳情…）：下載完再讀內容，用來把影片 ID 對應到它的檔案
            if (Uri.TryCreate(url, UriKind.Absolute, out var ru) && ru.Host.ToLowerInvariant() is var rh &&
                (rh == "douyin.com" || rh.EndsWith(".douyin.com", StringComparison.Ordinal)) && ru.AbsolutePath.Contains("/aweme/") &&
                mime.Contains("json") && (resp?["status"]?.GetValue<int>() ?? 0) < 400 && p?["requestId"]?.GetValue<string>() is { Length: > 0 } jreq)
            {
                lock (_gate)
                    if (_jsonSeen.Count < 20000 && _jsonSeen.Add(jreq)) _jsonReqs[jreq] = m["sessionId"]?.GetValue<string>() ?? "";
                return;
            }
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
    /// <param name="targetId">抖音影片 ID：有的話只認帶 data-e2e-vid=ID 的容器裡那支（不管是否正在播放）。</param>
    public async Task<double> QueryPlayingVideoAsync(int timeoutMs = 3000, string targetId = "")
    {
        // 除了 <video> 的狀態，也一併問頁面「這一篇」是不是抖音圖文貼文：圖片依頁面順序、背景音樂網址。
        // 音樂本身也是個 <video>，頁面還會預載別篇的影片與圖片，只看網路請求或 <video> 會抓錯。
        // 「這一篇」的容器：有指定 ID 時是帶 data-e2e-vid=ID 的容器（播完換下一支後仍留在頁面上）；沒指定時是 data-e2e=feed-active-video。
        var t = targetId.Length > 0 && targetId.All(char.IsAsciiDigit) ? targetId : "";
        var expression = "JSON.stringify((()=>{const T='" + t + "';" + """
            const els=T?[...document.querySelectorAll('[data-e2e-vid="'+T+'"]')]:[];
            const r=T?(els.find(e=>e.querySelector('video'))||els.find(e=>e.querySelector('img'))||null):document.querySelector('[data-e2e="feed-active-video"]');let post={};
            // 第 6 欄：在這一篇容器裡是 1（容器裡的背景音樂 <video> 是 2）、不在是 0、頁面沒有這個容器是 -1（有指定 ID 卻找不到容器也是 0：一支都不算，繼續等）
            const v=[...document.querySelectorAll('video')].map(v=>[Number.isFinite(v.duration)?v.duration:-1,v.paused?1:0,v.currentTime,v.videoWidth,v.videoHeight,r?(r.contains(v)?(/ies-music|\.mp3/.test(v.currentSrc||v.src||'')?2:1):0):(T?0:-1)]);
            if(r){const key=u=>{try{return new URL(u).pathname.split('~')[0]}catch(e){return ''}};const seen=new Set(),slides=[];
            r.querySelectorAll('img').forEach(i=>{if(/aweme-images/.test(i.src)&&!seen.has(key(i.src))){seen.add(key(i.src));slides.push({key:key(i.src),src:i.src})}});
            const m=[...r.querySelectorAll('video,audio')].map(v=>v.currentSrc||v.src||'').find(u=>/^https?:/.test(u))||'';post={slides:slides,music:m}}
            const fa=document.querySelector('[data-e2e="feed-active-video"]');
            return {v:v,post:post,act:fa?(fa.getAttribute('data-e2e-vid')||''):'',href:location.href}})())
            """;
        string[] targets;
        int gen;
        TaskCompletionSource<bool> done;
        lock (_gate)
        {
            gen = ++_queryGen;
            _videos.Clear();
            _post = null;
            _qAct = _qHref = "";
            LastActiveId = "";          // 每次查詢重新取得；查不到就是空的，不沿用上一篇
            LastPost = null;
            _pendingEval.Clear();
            _evalDone = done = new TaskCompletionSource<bool>(TaskCreationOptions.RunContinuationsAsynchronously);
            targets = [.. _sessions.Where(kv => kv.Value is "page" or "iframe").Select(kv => kv.Key)];
        }
        if (targets.Length == 0) return -1;
        var ids = targets.Select(_ => NewId()).ToArray();
        lock (_gate) foreach (var id in ids) _pendingEval.Add(id);
        try
        {
            for (var i = 0; i < targets.Length; i++)
                await SendAsync(ids[i], "Runtime.evaluate", new JsonObject { ["expression"] = expression, ["returnByValue"] = true }, targets[i]);
        }
        catch (Exception e) when (e is WebSocketException or OperationCanceledException)
        {
            lock (_gate) if (gen == _queryGen) _pendingEval.Clear();
            return -1;
        }
        await Task.WhenAny(done.Task, Task.Delay(timeoutMs)).ConfigureAwait(false);        // 有頁面沒回應也不能一直等
        double duration = -1, best = -1;
        var playingFound = false;
        string href;
        lock (_gate)
        {
            if (gen != _queryGen) return -1;            // 已經有更新的查詢開始了：這次的結果作廢
            _pendingEval.Clear();                       // 封存：之後才到的回覆一律不算（回覆處理整段都在 _gate 裡）
            LastActiveId = _qAct;
            LastPost = _post;
            href = _qHref;
            var haveRoot = _videos.Any(v => v.Length > 5 && v[5] >= 0);
            foreach (var v in _videos)
            {
                if (haveRoot && !(v.Length > 5 && v[5] == 1)) continue;
                var playing = v[1] == 0 && v[2] > 0.3;
                var score = (playing ? 1e6 : 0) + v[2];
                if (v[0] > 0 && score > best) { best = score; duration = v[0]; playingFound = playing; }
            }
        }
        LastQueryPlaying = playingFound;
        LastQueryTargetHit = t.Length > 0 && duration > 0;
        if (StartTargetId.Length == 0 && StartIsDouyin && Platforms.DouyinVideoId(href) is { Length: > 0 } hid)
        {
            StartTargetId = hid;      // 短連結轉址到 /video/ID（或 modal_id=ID）：第一次看到就固定，之後自動連播換片也不改
            Status?.Invoke("從轉址後的網址取得抖音影片 ID：" + hid);
        }
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
        var ordered = candidates.Where(c => c.Kind != "slide").OrderBy(c => c.Seq).ToList();
        if (ordered.FirstOrDefault(c => c.Kind == "stream") is { } stream) return [stream];
        if (targetDuration > 0)
        {
            var same = ordered.Where(c => CaptureUtil.UrlDuration(c.Url) is var d && (d < 0 || Math.Abs(d - targetDuration) <= 1.5)).ToList();
            if (same.Count > 0) ordered = same;
        }
        return ordered.Take(CaptureUtil.MaxFiles).ToList();
    }

    /// <summary>有這篇的抖音貼文資料：只下載屬於它的檔案（資料裡最高畫質的那個，加上偵測到、身分相符的）。
    /// 空的也不退回其他候選：寧可不下載，也不混進別篇的檔案。</summary>
    public static List<Candidate> PickDouyin(IEnumerable<Candidate> candidates, DouyinPost dp)
    {
        var mine = new List<Candidate>();
        var keys = new HashSet<string>();
        if (dp.Best is not null) { mine.Add(dp.Best); keys.Add(CaptureUtil.DouyinFileKey(dp.Best.Url)); }
        foreach (var c in candidates.OrderBy(c => c.Seq))
            if (CaptureUtil.DouyinFileKey(c.Url) is { Length: > 0 } k && dp.FileKeys.Contains(k) && keys.Add(k)) mine.Add(c);
        return mine.Take(4).ToList();
    }

    private static string SlideKey(string url) => (Uri.TryCreate(url, UriKind.Absolute, out var u) ? u.AbsolutePath : url).Split('~')[0];

    /// <summary>抖音圖文貼文：沒有任何影像／串流，只有圖片（slide）加音樂 → 依偵測順序的圖片與音樂；不是回傳 null。
    /// 同一張圖會以不同尺寸（~tplv-… 後綴不同）、不同簽章重複請求，用檔案路徑 ~ 之前的部分當身分。</summary>
    public static (List<Candidate> Slides, Candidate Music)? SlideshowParts(IEnumerable<Candidate> candidates, PostInfo? post = null)
    {
        var ordered = candidates.OrderBy(c => c.Seq).ToList();
        // 頁面說目前這一篇是圖文貼文：用頁面上的圖片網址與音樂，不管偵測到什麼影片（多半是預載的別篇）
        if (post is { Slides.Count: > 0 })
        {
            var referer = ordered.Select(c => c.Referer).FirstOrDefault(r => r != "") ?? "https://www.douyin.com/";
            var pm = post.Music != ""
                ? new Candidate { Url = post.Music, Kind = "audio", Referer = referer }
                : ordered.FirstOrDefault(c => c.Kind is "audio" or "media");
            return pm is null ? null : (post.Slides.Select((u, i) => new Candidate { Url = u, Mime = "image/webp", Kind = "slide", Referer = referer, Seq = i }).ToList(), pm);
        }
        if (ordered.Any(c => c.Kind is "stream" or "video")) return null;
        var slides = new Dictionary<string, Candidate>();
        foreach (var c in ordered.Where(c => c.Kind == "slide"))
        {
            var key = SlideKey(c.Url);
            if (!slides.TryGetValue(key, out var old)) slides[key] = c;
            else if (c.Size > old.Size)     // 同一張圖取最大的版本，位置仍是第一次出現的（複製一份，不動偵測階段的資料）
                slides[key] = new Candidate { Url = c.Url, Mime = old.Mime, Referer = old.Referer, Kind = old.Kind, Size = c.Size, Seq = old.Seq };
        }
        var music = ordered.FirstOrDefault(c => c.Kind is "audio" or "media");
        return slides.Count > 0 && music is not null ? (slides.Values.ToList(), music) : null;
    }

    private static readonly HttpClient SlideHttp = new() { Timeout = TimeSpan.FromSeconds(30) };

    /// <summary>圖片平均分配到音樂長度，和音樂合成 H.264 + AAC 的 mp4。與 shared 外掛 douyin_slideshow.py 同一套做法。</summary>
    private static async Task<bool> MakeSlideshowAsync(List<string> slides, string audio, string ffmpegDir, string output, Action<string> log)
    {
        async Task<(int Code, string Out, string Err)> RunAsync(string exe, IEnumerable<string> args)
        {
            using var p = Core.CreateProcess(Path.Combine(ffmpegDir, exe), args);
            try { p.Start(); }
            catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException) { return (-1, "", e.Message); }
            var o = p.StandardOutput.ReadToEndAsync();
            var er = p.StandardError.ReadToEndAsync();
            await p.WaitForExitAsync().ConfigureAwait(false);
            return (p.ExitCode, await o.ConfigureAwait(false), await er.ConfigureAwait(false));
        }
        JsonNode? Parse(string s) { try { return JsonNode.Parse(s); } catch (System.Text.Json.JsonException) { return null; } }

        var probeA = Parse((await RunAsync("ffprobe.exe", ["-v", "error", "-print_format", "json", "-show_entries", "format=duration", audio]).ConfigureAwait(false)).Out);
        _ = double.TryParse(probeA?["format"]?["duration"]?.ToString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var total);
        var each = total > 0 ? Math.Max(total / slides.Count, 1.0) : 3.0;
        // 畫布 = 第一張圖的實際尺寸（長寬取偶數，最長邊不超過 4096），不縮小高解析的圖；讀不到尺寸用 1080x1920
        int w = 1080, h = 1920;
        var probeI = Parse((await RunAsync("ffprobe.exe", ["-v", "error", "-print_format", "json", "-show_entries", "stream=width,height", slides[0]]).ConfigureAwait(false)).Out);
        if (probeI?["streams"] is JsonArray arr && arr.Count > 0 && (arr[0]?["width"]?.GetValue<int>() ?? 0) > 0)
        {
            int iw = arr[0]!["width"]!.GetValue<int>(), ih = arr[0]?["height"]?.GetValue<int>() ?? 0;
            var k = Math.Min(1.0, 4096.0 / Math.Max(iw, ih));
            w = Math.Max(2, (int)(iw * k) / 2 * 2);
            h = Math.Max(2, (int)(ih * k) / 2 * 2);
        }
        var fit = $"scale={w}:{h}:force_original_aspect_ratio=decrease,pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps=30,format=yuv420p";
        var n = slides.Count;
        var graph = string.Concat(Enumerable.Range(0, n).Select(i => $"[{i}:v]{fit}[v{i}];")) +
                    string.Concat(Enumerable.Range(0, n).Select(i => $"[v{i}]")) + $"concat=n={n}:v=1:a=0[v]";
        var a = new List<string> { "-y", "-loglevel", "error" };
        foreach (var f in slides) a.AddRange(["-loop", "1", "-t", each.ToString("F3", CultureInfo.InvariantCulture), "-i", f]);
        a.AddRange(["-i", audio, "-filter_complex", graph, "-map", "[v]", "-map", $"{n}:a:0", "-c:v", "libx264", "-preset", "fast", "-crf", "20",
                    "-c:a", "aac", "-b:a", "192k"]);
        if (total > 0) a.AddRange(["-t", total.ToString("F3", CultureInfo.InvariantCulture)]); else a.Add("-shortest");   // -shortest 遇到真實的抖音音樂會把聲音截短
        a.AddRange(["-movflags", "+faststart", output]);
        var r = await RunAsync("ffmpeg.exe", a).ConfigureAwait(false);
        if (r.Code != 0 || !File.Exists(output))
        {
            log("合成圖文影片失敗：" + (r.Err.Trim().Length > 300 ? r.Err.Trim()[^300..] : r.Err.Trim()));
            return false;
        }
        return true;
    }

    /// <summary>抖音圖文貼文：下載圖片與音樂，合成 mp4。回傳結果路徑；失敗或停止回傳 null。</summary>
    private static async Task<string?> DownloadSlideshowAsync(
        List<Candidate> slides, Candidate music, string outDir, Engine engine, string? ffmpegDir,
        Action<string> log, Action<double>? progress, Action<string>? status, CancellationToken ct)
    {
        if (string.IsNullOrEmpty(ffmpegDir)) { log("找不到 ffmpeg，無法合成圖文影片。"); return null; }
        Directory.CreateDirectory(outDir);
        var stamp = DateTime.Now.ToString("yyyyMMdd_HHmmss", CultureInfo.InvariantCulture);
        var work = Path.Combine(Path.GetTempPath(), "avd-slides-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(work);
        try
        {
            log($"抖音圖文貼文：{slides.Count} 張圖片 + 音樂");
            status?.Invoke($"下載 {slides.Count} 張圖片…");
            var files = new List<string>();
            for (var i = 0; i < slides.Count; i++)
            {
                if (ct.IsCancellationRequested) return null;
                try
                {
                    using var req = new HttpRequestMessage(HttpMethod.Get, slides[i].Url);
                    req.Headers.TryAddWithoutValidation("Referer", string.IsNullOrEmpty(slides[i].Referer) ? "https://www.douyin.com/" : slides[i].Referer);
                    req.Headers.TryAddWithoutValidation("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36");
                    using var resp = await SlideHttp.SendAsync(req, ct).ConfigureAwait(false);
                    resp.EnsureSuccessStatusCode();
                    var f = Path.Combine(work, $"{i + 1:D3}.img");
                    await File.WriteAllBytesAsync(f, await resp.Content.ReadAsByteArrayAsync(ct).ConfigureAwait(false), ct).ConfigureAwait(false);
                    files.Add(f);
                }
                catch (Exception e) when (e is HttpRequestException or TaskCanceledException or IOException)
                {
                    if (ct.IsCancellationRequested) return null;
                    log($"第 {i + 1} 張圖片下載失敗，略過：{e.Message}");
                }
                progress?.Invoke((i + 1) * 70.0 / slides.Count);
            }
            if (files.Count == 0) { log("沒有下載到任何圖片。"); return null; }
            status?.Invoke("下載音樂…");
            var (code, audio) = await DownloadOneAsync(music, Path.Combine(work, "music.%(ext)s"), engine, ffmpegDir, true, null, ct).ConfigureAwait(false);
            if (code != 0) (code, audio) = await DownloadOneAsync(music, Path.Combine(work, "music.%(ext)s"), engine, ffmpegDir, false, null, ct).ConfigureAwait(false);
            if (ct.IsCancellationRequested) return null;
            if (code != 0 || audio is null) { log("音樂下載失敗。"); return null; }
            progress?.Invoke(85);
            status?.Invoke("合成影片…");
            var output = Path.Combine(outDir, $"擷取_{stamp}.mp4");
            if (!await MakeSlideshowAsync(files, audio, ffmpegDir, output, log).ConfigureAwait(false)) return null;
            progress?.Invoke(100);
            log("完成：" + output);
            return output;
        }
        finally { try { Directory.Delete(work, true); } catch (IOException) { } }
    }

    public static async Task<string?> DownloadAndMergeAsync(
        IEnumerable<Candidate> candidates, double targetDuration, string outDir, Engine engine, string? ffmpegDir,
        Action<string> log, Action<double>? progress = null, Action<string>? status = null, CancellationToken ct = default,
        PostInfo? post = null, DouyinPost? douyin = null)
    {
        var candidateList = candidates.ToList();
        List<Candidate> queue;
        if (douyin is not null)
        {
            // 有這篇的貼文資料：是不是圖文、要下載哪些檔案都以資料為準（頁面上的判斷可能是別篇）
            if (douyin.Post is { } dpost)
            {
                log($"抖音圖文 {douyin.Id}：依貼文資料取 {dpost.Slides.Count} 張圖片與音樂");
                // 只有資料裡有音樂才成立：不拿別篇的音樂
                if (dpost.Music.Length == 0 || SlideshowParts([], dpost) is not { } dparts)
                {
                    log($"抖音貼文 {douyin.Id} 的資料裡沒有音樂，沒有下載別篇的檔案。");
                    return null;
                }
                return await DownloadSlideshowAsync(dparts.Slides, dparts.Music, outDir, engine, ffmpegDir, log, progress, status, ct).ConfigureAwait(false);
            }
            queue = PickDouyin(candidateList, douyin);
            if (queue.Count == 0)
            {
                log($"抖音貼文 {douyin.Id} 的資料裡沒有可下載的影片，沒有下載別篇的檔案。");
                return null;
            }
            log($"抖音影片 {douyin.Id}：依貼文資料只下載屬於它的檔案（最高畫質 {douyin.Width}×{douyin.Height}）");
        }
        else
        {
            if (SlideshowParts(candidateList, post) is { } parts)
                return await DownloadSlideshowAsync(parts.Slides, parts.Music, outDir, engine, ffmpegDir, log, progress, status, ct).ConfigureAwait(false);
            queue = PickCandidates(candidateList, targetDuration);
        }
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
        var slideshow = false;
        PostInfo? post = null;
        var postSig = "";
        var postClock = Stopwatch.StartNew();
        var hit = false;
        DouyinPost? dp = null;
        // 網址指定那篇的貼文資料：有可下載的影片，或是圖文（圖文缺音樂在下載時明確失敗，不拿別篇的）
        DouyinPost? ReadyPost(string id) => session.GetDouyinPost(id) is { } d && (d.Best is not null || d.Post is not null) ? d : null;
        while (DateTime.UtcNow < deadline && !ct.IsCancellationRequested)
        {
            var tid = session.StartTargetId;          // 短連結：轉址後才有
            // 已經拿到網址指定那篇的貼文資料：長度、檔案網址（或圖文的圖片與音樂）都有了，不必等播放，直接下載
            if ((dp = ReadyPost(tid)) is not null) break;
            // 有指定抖音影片 ID 就不等偵測到影音才問：網路偵測常比實際播放晚好幾秒，短影片可能已經播完換下一支
            // （抖音短連結還沒取得 ID 時也先問：轉址後的網址會帶 ID）
            if (session.Candidates.Count > 0 || tid.Length > 0 || session.StartIsDouyin)
            {
                var d = await session.QueryPlayingVideoAsync(2000, tid).ConfigureAwait(false);
                if (tid.Length == 0 && session.StartTargetId.Length > 0) continue;   // 短連結這次查詢才取得 ID：這次結果不是針對那篇，改用 ID 重查
                // 抖音圖文貼文：頁面說目前這篇有圖片就不再看其他影片（音樂也是 <video>，別篇預載的影片也可能「正在播放」）；圖片清單穩定一陣子就開始
                if (session.LastPost is { } lp)
                {
                    var sig = string.Join("|", lp.Slides);
                    if (sig != postSig) { postSig = sig; postClock.Restart(); }
                    else if (postClock.ElapsedMilliseconds >= AutoSettleMs && SlideshowParts(session.Candidates, lp) is not null) { slideshow = true; post = lp; break; }
                    await Task.Delay(500, CancellationToken.None).ConfigureAwait(false);
                    continue;
                }
                if (d > 0 && (session.LastQueryPlaying || session.LastQueryTargetHit)) { target = d; hit = session.LastQueryTargetHit; break; }
            }
            await Task.Delay(500, CancellationToken.None).ConfigureAwait(false);
        }
        if (ct.IsCancellationRequested) return (null, "cancelled", target);
        var targetId = session.StartTargetId;
        if (dp is not null)
        {
            if (dp.Post is null)
            {
                target = dp.Duration;
                log($"從抖音貼文資料鎖定影片 {dp.Id}：{target.ToString("F2", CultureInfo.InvariantCulture)} 秒");
            }
        }
        else if (!slideshow)
        {
            if (target <= 0) return (null, "manual", target);
            log(hit ? $"鎖定網址指定的抖音影片（{targetId}）長度：{target.ToString("F2", CultureInfo.InvariantCulture)} 秒"
                    : $"鎖定正在播放的影片長度：{target.ToString("F2", CultureInfo.InvariantCulture)} 秒");
            status?.Invoke("等待影像與聲音都出現…");
            // 鎖定可能早於偵測到任何影音（有指定 ID 時不等候選就先問）：至少要有一個候選，才開始算「穩定」。
            // 等候期間拿到這篇的貼文資料就不必再等：檔案網址已知。
            var waitDeadline = DateTime.UtcNow.AddSeconds(AutoWaitSeconds);
            while (session.Candidates.Count == 0 && ReadyPost(targetId) is null && DateTime.UtcNow < waitDeadline && !ct.IsCancellationRequested)
                await Task.Delay(200, CancellationToken.None).ConfigureAwait(false);
            if (ct.IsCancellationRequested) return (null, "cancelled", target);
            if (session.Candidates.Count == 0 && ReadyPost(targetId) is null) return (null, "manual", target);
            var count = session.Candidates.Count;
            var changed = Stopwatch.StartNew();
            var settle = Stopwatch.StartNew();
            while (settle.ElapsedMilliseconds < 8000 && !ct.IsCancellationRequested && ReadyPost(targetId) is null)
            {
                if (session.Candidates.Count != count) { count = session.Candidates.Count; changed.Restart(); }
                else if (changed.ElapsedMilliseconds >= AutoSettleMs) break;
                await Task.Delay(200, CancellationToken.None).ConfigureAwait(false);
            }
            if (ct.IsCancellationRequested) return (null, "cancelled", target);
            // 有指定 ID 就一定以這篇的貼文資料為準（有就用，不靠長度挑）
            dp = targetId.Length > 0 ? ReadyPost(targetId) : null;
            if (targetId.Length > 0 && dp is null) log($"沒有取得抖音影片 {targetId} 的貼文資料，改用長度判斷");
        }
        if (dp?.Post is not null || slideshow)
        {
            // 圖文：依貼文資料（有的話）或頁面上的圖片合成；沒有影片長度可比對
            var slidePath = await DownloadAndMergeAsync(session.Candidates, -1, outDir, engine, ffmpegDir, log, progress, status, ct,
                                                        dp?.Post is not null ? null : post,
                                                        dp ?? (targetId.Length > 0 ? session.GetDouyinPost(targetId) : null)).ConfigureAwait(false);
            if (ct.IsCancellationRequested) return (null, "cancelled", target);
            return slidePath is null ? (null, "failed", target) : (slidePath, "ok", target);
        }
        var path = await DownloadAndMergeAsync(session.Candidates, target, outDir, engine, ffmpegDir, log, progress, status, ct, douyin: dp).ConfigureAwait(false);
        if (ct.IsCancellationRequested) return (null, "cancelled", target);
        if (path is null) return (null, "failed", target);
        if (target <= 0)
        {
            log("貼文資料沒有影片長度，略過長度比對");      // 檔案本身就是依貼文資料挑的
            return (path, "ok", target);
        }
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
        return await DownloadAndMergeAsync(session.Candidates, target, outDir, engine, ffmpegDir, log, post: session.LastPost,
                                           douyin: session.GetDouyinPost(session.LastActiveId)).ConfigureAwait(false);
    }
}
