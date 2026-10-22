// 下載核心：找 yt-dlp、組命令列參數、執行下載／解析／更新。與介面無關。
//
// 規則與 py/anyvideo_download.py 的 build_ydl_options() 相同，只是 Python 版直接呼叫
// yt-dlp 函式庫，這裡呼叫 yt-dlp 命令列。Python 版自己插入的 EnsureH264PP，這裡由
// yt-dlp 外掛（shared/yt-dlp-plugins）提供。
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json.Nodes;
using System.Text.RegularExpressions;

namespace AnyVideoDownload;

public enum EngineKind { None, Exe, PyModule }

/// <param name="Program">yt-dlp.exe 或 py.exe</param>
/// <param name="Prefix">PyModule 時是 -3 -m yt_dlp</param>
public sealed record Engine(EngineKind Kind, string Program, IReadOnlyList<string> Prefix, string Version)
{
    public static readonly Engine None = new(EngineKind.None, "", [], "");
    public bool Ok => Kind != EngineKind.None;
    public IEnumerable<string> Args(IEnumerable<string> rest) => Prefix.Concat(rest);
}

public sealed record DownloadResult(int Ok, int Fail, bool Cancelled);

/// <summary>失敗後「換個方式再試」的調整，見 Core.PlanRetry。預設值就是第一次嘗試。</summary>
public struct RetryTweaks
{
    public bool RelaxFormat;    // 放掉畫質／編碼排序，用寬鬆的格式選擇
    public bool Impersonate;    // 以 Chrome 瀏覽器身分連線（需要 curl_cffi）
}

public static partial class Core
{
    // yt-dlp 的進度改用這兩個樣板輸出，一行一筆，欄位用 | 分隔；沒有值的欄位會印成 NA。
    // 以 AVD- 開頭，和 yt-dlp 自己的訊息分得開。
    private const string DownloadTemplate =
        "download:AVD-DL|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s" +
        "|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s" +
        "|%(progress.fragment_index)s|%(progress.fragment_count)s" +
        "|%(info.vcodec)s|%(info.acodec)s|%(info.playlist_index)s|%(info.n_entries)s";
    private const string PostprocessTemplate = "postprocess:AVD-PP|%(progress.status)s|%(progress.postprocessor)s";

    internal static readonly string[] PipInstall = ["-m", "pip", "install", "-U", "yt-dlp[default,curl-cffi]"];

    [GeneratedRegex(@"^\d{4}\.\d")]
    private static partial Regex LooksLikeVersion();

    // ─── 環境偵測 ───────────────────────────────────────────────────────────

    /// <summary>
    /// 依序找：程式目錄的 yt-dlp.exe → PATH 上的 yt-dlp.exe → py -3 -m yt_dlp。
    /// 每個候選都實際跑一次 --version。
    /// </summary>
    public static async Task<Engine> DetectEngineAsync()
    {
        var exes = new List<string>();
        var local = Path.Combine(AppContext.BaseDirectory, "yt-dlp.exe");
        if (File.Exists(local)) exes.Add(local);
        if (FindExecutable("yt-dlp") is { } onPath) exes.Add(onPath);

        foreach (var exe in exes)
        {
            var v = await VersionOfAsync(exe, ["--version"]).ConfigureAwait(false);
            if (v is not null) return new Engine(EngineKind.Exe, exe, [], v);
        }
        if (FindExecutable("py") is { } py)
        {
            string[] prefix = ["-3", "-m", "yt_dlp"];
            var v = await VersionOfAsync(py, [.. prefix, "--version"]).ConfigureAwait(false);
            if (v is not null) return new Engine(EngineKind.PyModule, py, prefix, v);
        }
        return Engine.None;
    }

    private static async Task<string?> VersionOfAsync(string program, IEnumerable<string> args)
    {
        using var p = CreateProcess(program, args);
        try
        {
            p.Start();
        }
        catch (Exception)
        {
            return null;
        }
        var stdout = p.StandardOutput.ReadToEndAsync();
        var stderr = p.StandardError.ReadToEndAsync();
        using var timeout = new CancellationTokenSource(TimeSpan.FromSeconds(60));
        try
        {
            await p.WaitForExitAsync(timeout.Token).ConfigureAwait(false);
        }
        catch (OperationCanceledException)
        {
            KillTree(p);
            return null;
        }
        await Task.WhenAll(stdout, stderr).ConfigureAwait(false);
        if (p.ExitCode != 0) return null;
        var v = stdout.Result.Trim().Split('\n')[0].Trim();
        return LooksLikeVersion().IsMatch(v) ? v : null;
    }

    /// <summary>在 PATH 裡找執行檔，副檔名依 PATHEXT。找不到回傳 null。</summary>
    public static string? FindExecutable(string name)
    {
        var exts = (Environment.GetEnvironmentVariable("PATHEXT") ?? ".EXE")
            .Split(';', StringSplitOptions.RemoveEmptyEntries);
        var dirs = (Environment.GetEnvironmentVariable("PATH") ?? "")
            .Split(';', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        foreach (var dir in dirs)
        {
            foreach (var ext in exts)
            {
                try
                {
                    var candidate = Path.Combine(dir.Trim('"'), name + ext.ToLowerInvariant());
                    if (File.Exists(candidate)) return Path.GetFullPath(candidate);
                }
                catch (ArgumentException)
                {
                    // PATH 裡有不合法的路徑，略過
                }
            }
        }
        return null;
    }

    // ─── 失敗原因判斷 ───────────────────────────────────────────────────────

    // 順序重要：先比對的優先。與 py/anyvideo_download.py 的 _ERROR_KINDS、qt/src/engine.cpp 相同。
    private static readonly (string Kind, Regex Pattern)[] ErrorKinds =
    [
        ("drm", new("DRM")),
        ("disk", new("No space left|Permission denied|File name too long|path too long|Errno 28|Errno 13")),
        ("bot", new("confirm you.re not a bot|Sign in to confirm")),
        ("login", new("[Ll]og(?:ged)?[- ]?in|[Ss]ign in|private video|members-only|[Cc]ookies|requires authentication|HTTP Error 401")),
        ("geo", new("geo[- ]?restrict|not available in your country|in your location")),
        ("impersonate", new(@"impersonat|HTTP Error 403|HTTP Error 429|Cloudflare|Just a moment|\b403\b")),
        // 憑證驗證失敗不是網路不穩：重試一百次結果都一樣，所以放在 network 之前，單獨一類、不重試
        ("cert", new("CERTIFICATE_VERIFY_FAILED|certificate verify failed|self[- ]signed certificate|unable to get local issuer")),
        ("format", new("[Rr]equested format is not available|No video formats found")),
        ("network", new(@"timed? ?out|Temporary failure|Connection (?:reset|aborted|refused)|IncompleteRead|RemoteDisconnected|Name or service not known|getaddrinfo|SSL|HTTP Error 5\d\d|Unable to download (?:webpage|JSON)|EOF occurred")),
        ("ffmpeg", new("ffmpeg|ffprobe|Postprocessing")),
        ("parse", new("Cannot parse data")),
        ("update", new("Unsupported URL|Unable to extract|nsig|signature|player response|Failed to parse JSON|Unable to download.*(?:API|player)|HTTP Error 404")),
    ];

    public static string ClassifyError(string text)
    {
        foreach (var (kind, pattern) in ErrorKinds)
            if (pattern.IsMatch(text)) return kind;
        return "unknown";
    }

    /// <summary>失敗原因 → 使用者看得懂、做得到的下一步；沒有建議回傳 null。</summary>
    public static string? ErrorHint(string kind, Settings s) => kind switch
    {
        "drm" => "這個內容有 DRM 保護，無法下載。",
        "disk" => "寫入失敗：請確認儲存位置還有空間、有寫入權限，且路徑不要太深。",
        "bot" => "網站要求證明不是機器人。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。",
        "login" => s.Browser == "不使用"
            ? "這個內容需要登入。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。"
            : "已使用瀏覽器 cookies 仍要求登入：請確認有在該瀏覽器登入，並先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies）。",
        "geo" => "網站限制地區，需要該地區的網路（VPN／代理）才能下載。",
        "impersonate" => "網站擋掉了程式的連線。若尚未安裝瀏覽器偽裝元件 curl_cffi，按「更新下載核心」安裝後再試。",
        "cert" => "HTTPS 憑證驗證失敗：這台電腦連到這個網站時，拿到的憑證不是網站本身的（網路供應商、公司防火牆、防毒軟體的 HTTPS 掃描，或網站被導向到別的伺服器）。重試沒有用，請檢查網路環境；不要為了下載而關閉憑證檢查。",
        "network" => "網路連線不穩或被中斷，已自動重試仍失敗。請檢查網路／VPN 後再試，續傳會接著下載。",
        "ffmpeg" => "ffmpeg 處理失敗：確認 ffmpeg 可用（winget install Gyan.FFmpeg），或換一種輸出格式。",
        "parse" => "網站回傳的頁面解析不了。最常見是私人／朋友限定／社團內容（Facebook、Instagram 等）：「登入狀態」選一個已登入該網站的瀏覽器再試；公開內容仍失敗才是網站改版，請更新下載核心。",
        "update" => "網站可能改版了：按「更新下載核心」，重新開啟程式再試。",
        _ => null,
    };

    /// <summary>
    /// 這類失敗值得換個方式再試嗎？值得就回傳 true 並填入 tweaks／message；tried 記錄已試過的類別，每種辦法只試一次。
    /// </summary>
    public static bool PlanRetry(string kind, HashSet<string> tried, ref RetryTweaks tweaks, out string message)
    {
        message = "";
        if (tried.Contains(kind)) return false;
        switch (kind)
        {
            case "network": message = "網路不穩，稍後重試…"; break;
            case "format": tweaks.RelaxFormat = true; message = "找不到符合的格式，改用寬鬆的格式選擇重試…"; break;
            case "impersonate": tweaks.Impersonate = true; message = "改以 Chrome 瀏覽器身分連線重試…"; break;
            default: return false;
        }
        tried.Add(kind);
        return true;
    }

    public static string? FindFfmpegDir() =>
        FindExecutable("ffmpeg") is { } exe ? Path.GetDirectoryName(exe) : null;

    public static bool HasDeno() => FindExecutable("deno") is not null;

    public static string PluginDir => Path.Combine(AppContext.BaseDirectory, "yt-dlp-plugins");

    /// <summary>
    /// 外掛確實在 <see cref="PluginDir"/> 底下時回傳它，否則回傳 null。
    /// 資料夾不存在時不能把它交給 --plugin-dirs：yt-dlp 會丟出
    /// 「Invalid plugin directory」直接結束，每一個網址都下載失敗。
    /// </summary>
    public static string? PluginDirIfPresent =>
        File.Exists(Path.Combine(PluginDir, "anyvideo", "yt_dlp_plugins", "postprocessor", "ensure_h264.py"))
            ? PluginDir
            : null;

    /// <summary>這組設定會不會用到 EnsureH264 外掛（相容優先，或輸出 mov）。</summary>
    public static bool NeedsEnsureH264(Settings s) =>
        s.Mode == "video" && s.Priority != "original" &&
        (s.Container == "mov" || (s.Priority == "compat" && Options.CompatH264Containers.Contains(s.Container)));

    // ─── 命令列參數 ─────────────────────────────────────────────────────────

    /// <param name="pluginDir">null 或空字串表示不帶 --plugin-dirs。</param>
    /// <param name="tweaks">失敗後「換個方式再試」的調整（見 PlanRetry）；預設就是第一次嘗試。</param>
    public static List<string> BuildDownloadArgs(Settings s, string? ffmpegDir, string? pluginDir, string url,
        RetryTweaks tweaks = default)
    {
        // --ignore-config：不讓使用者自己的 yt-dlp 設定檔改變結果
        var a = new List<string> { "--ignore-config", "--newline", "--color", "never" };
        a.Add(s.Playlist ? "--yes-playlist" : "--no-playlist");
        // 命令列預設會把 multi_video 清單合併成一個檔；Python 版（函式庫）不會，照 Python 版
        a.AddRange(["--concat-playlist", "never"]);

        // .150B 是以位元組截斷標題，避免超過 Windows 的路徑長度限制
        var tmpl = s.Playlist
            ? @"%(playlist_title,playlist|playlist).80B\%(playlist_index|0)03d - %(title).150B [%(id)s].%(ext)s"
            : "%(title).150B [%(id)s].%(ext)s";
        var outDir = s.OutDir.Replace('/', '\\').TrimEnd('\\');
        a.AddRange(["-o", outDir + "\\" + tmpl]);

        a.AddRange(["--windows-filenames", "--retries", "10", "--fragment-retries", "10", "--extractor-retries", "5",
            "--socket-timeout", "30", "--retry-sleep", "http:exp=1:20", "--retry-sleep", "fragment:exp=1:20",
            "--concurrent-fragments", "4"]);
        if (!string.IsNullOrEmpty(ffmpegDir)) a.AddRange(["--ffmpeg-location", ffmpegDir]);
        var browser = Options.ValueOf(Options.Browsers, s.Browser);
        if (browser != "") a.AddRange(["--cookies-from-browser", browser]);
        if (!string.IsNullOrEmpty(pluginDir)) a.AddRange(["--plugin-dirs", pluginDir]);
        if (tweaks.Impersonate) a.AddRange(["--impersonate", "chrome"]);
        a.AddRange(["--progress-template", DownloadTemplate, "--progress-template", PostprocessTemplate]);

        string target;
        if (s.Mode == "video")
        {
            var c = s.Container;
            var h = Options.ValueOf(Options.VideoQualities, s.Quality);
            var hf = h == "" ? "" : $"[height<={h}]";
            a.AddRange(["-f", tweaks.RelaxFormat ? "bv*+ba/b" : $"bestvideo*{hf}+bestaudio/best{hf}/best"]);

            if (s.Priority == "original")
            {
                // 原始品質：不指定排序（yt-dlp 自己的順序就是畫質優先：解析度、幀率、HDR、編碼、位元率，聲音挑位元率最高）、
                // 不轉檔、不重新封裝成別的容器、不碰 EnsureH264。合併只是把影像與聲音串流複製進同一個容器：
                // 兩邊都放得進 mp4 就用 mp4，否則 mkv（什麼都裝得下）。結果與網站提供的串流解碼後逐格相同。
                a.AddRange(["--merge-output-format", "mp4/mkv"]);
                if (s.EmbedSubs) a.AddRange(["--embed-subs", "--sub-langs", "zh.*,en.*"]);
                if (s.EmbedMeta) a.AddRange(["--embed-metadata", "--embed-chapters"]);
                a.AddRange(["--", url]);
                return a;
            }

            // 畫質優先先比解析度；相容優先先比編碼（vcodec:h264 把 H.264 排第一）。
            // 詳細說明見 py/anyvideo_download.py 的 build_ydl_options()。
            var compat = s.Priority == "compat";
            var sort = compat
                ? (c == "webm" ? "vcodec:vp9,acodec:opus,res,ext:webm:webm" : "vcodec:h264,acodec:aac,res,ext:mp4:m4a")
                : c switch
                {
                    "webm" => "res,ext:webm:webm",
                    "mkv" => "res",
                    "mov" => "res,vcodec:h264,acodec:aac,ext:mp4:m4a",  // mov 裝不下 AV1／VP9，同解析度挑 H.264
                    _ => "res,ext:mp4:m4a",
                };
            if (!tweaks.RelaxFormat) a.AddRange(["-S", sort]);
            a.AddRange(["--merge-output-format", c switch { "mkv" => "mkv", "webm" => "webm/mkv", _ => "mp4/mkv" }]);

            // yt-dlp 命令列的後處理順序固定是：--use-postprocessor → 重新封裝 → 轉檔 →
            // 字幕 → 資訊 → 封面。EnsureH264 正好排在最前面。
            if (c == "mov" || (compat && Options.CompatH264Containers.Contains(c)))
            {
                a.AddRange(["--use-postprocessor", "EnsureH264:video_ok=" + (compat ? "h264" : "h264,hevc")]);
                a.AddRange(["--remux-video", c]);
            }
            else if (c == "mkv")
            {
                a.AddRange(["--remux-video", "mkv"]);
            }
            else
            {
                a.AddRange(["--recode-video", c]);
            }
            if (s.EmbedSubs && Options.SubsOk.Contains(c)) a.AddRange(["--embed-subs", "--sub-langs", "zh.*,en.*"]);
            target = c;
        }
        else
        {
            a.AddRange(["-f", "bestaudio/best", "-x", "--audio-format", s.AudioFormat == "ogg" ? "vorbis" : s.AudioFormat]);
            // 「原始（不轉檔）」= --audio-format best：聲音串流原封不動存成它原本的格式，沒有重新編碼
            if (!Options.LosslessAudio.Contains(s.AudioFormat) && s.AudioFormat != Options.AudioOriginalValue)
                a.AddRange(["--audio-quality", s.AudioBitrate]);
            target = s.AudioFormat;
        }

        if (s.EmbedMeta) a.AddRange(["--embed-metadata", "--embed-chapters"]);
        if (s.EmbedThumb && Options.ThumbOk.Contains(target)) a.Add("--embed-thumbnail");
        a.AddRange(["--", url]);
        return a;
    }

    public static List<string> BuildProbeArgs(Settings s, string url)
    {
        var a = new List<string> { "--ignore-config", "--color", "never", "-J", "--flat-playlist" };
        a.Add(s.Playlist ? "--yes-playlist" : "--no-playlist");
        var browser = Options.ValueOf(Options.Browsers, s.Browser);
        if (browser != "") a.AddRange(["--cookies-from-browser", browser]);
        a.AddRange(["--", url]);
        return a;
    }

    // ─── 小工具 ─────────────────────────────────────────────────────────────

    /// <param name="n">小於等於 0 表示未知</param>
    public static string FmtBytes(double n)
    {
        if (n <= 0) return "?";
        foreach (var unit in new[] { "B", "KB", "MB", "GB" })
        {
            if (n < 1024) return n.ToString("F1", CultureInfo.InvariantCulture) + " " + unit;
            n /= 1024;
        }
        return n.ToString("F1", CultureInfo.InvariantCulture) + " TB";
    }

    /// <param name="sec">小於 0 表示未知</param>
    public static string FmtEta(double sec)
    {
        if (sec < 0) return "?";
        var t = (long)sec;
        return t >= 3600 ? $"{t / 3600}:{t / 60 % 60:D2}:{t % 60:D2}" : $"{t / 60}:{t % 60:D2}";
    }

    internal static double Num(string? s) =>
        double.TryParse(s, NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : -1;

    /// <summary>子程序共同設定：UTF-8 輸出、不彈主控台視窗、參數逐一傳遞（不必自己處理引號）。</summary>
    public static Process CreateProcess(string program, IEnumerable<string> args)
    {
        var psi = new ProcessStartInfo(program)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            StandardOutputEncoding = new UTF8Encoding(false),
            StandardErrorEncoding = new UTF8Encoding(false),
        };
        foreach (var arg in args) psi.ArgumentList.Add(arg);
        psi.Environment["PYTHONIOENCODING"] = "utf-8";
        psi.Environment["PYTHONUTF8"] = "1";
        return new Process { StartInfo = psi };
    }

    /// <summary>yt-dlp 會再開 ffmpeg，只殺 yt-dlp 會留下孤兒 ffmpeg，所以整棵程序樹一起結束。</summary>
    public static void KillTree(Process p)
    {
        try
        {
            if (!p.HasExited) p.Kill(entireProcessTree: true);
        }
        catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception)
        {
            // 程序剛好自己結束了
        }
    }
}

/// <summary>
/// 背景工作的共同基底：事件一律回到建立物件時的執行緒（WPF 介面執行緒）觸發；
/// 沒有同步內容（無介面模式）時就地觸發，並用鎖保證同一時間只有一個事件在跑。
/// </summary>
public abstract class JobBase
{
    private readonly SynchronizationContext? _ctx = SynchronizationContext.Current;
    private readonly object _gate = new();

    public event Action<string, string>? Log;

    protected void Raise(Action action)
    {
        if (_ctx is null)
        {
            lock (_gate) action();
        }
        else
        {
            _ctx.Post(_ => action(), null);
        }
    }

    protected void RaiseLog(string text, string tag) => Raise(() => Log?.Invoke(text, tag));
}

/// <summary>依序下載多個網址，每個網址一個 yt-dlp 程序。</summary>
public sealed class DownloadJob(IReadOnlyList<string> urls, Settings settings, Engine engine, string? ffmpegDir)
    : JobBase
{
    private readonly object _lineGate = new();
    private readonly Stopwatch _lastPost = new();
    private readonly List<string> _errors = [];     // 這一次嘗試印出的 ERROR 行，失敗時用來判斷原因
    private readonly List<string> _captureUrls = [];

    /// <summary>失敗了、而且改用「從瀏覽器擷取」常常有效的網址（抖音、小紅書、TikTok）。</summary>
    public IReadOnlyList<string> CaptureCandidates => _captureUrls;
    private bool _stopping;

    public event Action<int, int, string>? Item;
    public event Action<double, string>? Progress;  // pct < 0：沒有百分比
    public event Action<string>? Status;           // 後處理中

    public async Task<DownloadResult> RunAsync(CancellationToken ct)
    {
        int ok = 0, fail = 0;
        // 需要外掛卻找不到時，在開始前就說清楚。交給 yt-dlp 的話，它只會回報
        // 「Unknown postprocessor」或丟出 Python traceback，使用者看不出原因。
        if (Core.NeedsEnsureH264(settings) && Core.PluginDirIfPresent is null)
        {
            RaiseLog($"找不到 yt-dlp 外掛：{Core.PluginDir}\n" +
                     "「相容優先」與輸出 mov 需要它。請把建置輸出的 yt-dlp-plugins 資料夾放在 " +
                     "AnyVideoDownload.exe 旁邊（重新執行 build.bat 會自動複製），" +
                     "或改用「畫質優先」並選 mov 以外的格式。", "error");
            return new(0, urls.Count, false);
        }
        for (var i = 0; i < urls.Count; i++)
        {
            if (ct.IsCancellationRequested) return new(ok, fail, true);
            var url = urls[i];
            var index = i + 1;
            Raise(() => Item?.Invoke(index, urls.Count, url));

            // 改寫成 yt-dlp 認得的網址（抖音的 modal_id、小紅書的用戶頁網址…），短連結先展開
            var (finalUrl, note) = await Platforms.ResolveAsync(url, null, ct).ConfigureAwait(false);
            if (note.Length > 0) RaiseLog(note, "info");
            if (ct.IsCancellationRequested) return new(ok, fail, true);

            // 失敗時依原因換個方式再試（見 Core.PlanRetry），每種辦法只試一次
            var tried = new HashSet<string>();
            var tweaks = new RetryTweaks();
            var success = false;
            while (true)
            {
                lock (_lineGate) _errors.Clear();
                using var p = Core.CreateProcess(engine.Program,
                    engine.Args(Core.BuildDownloadArgs(settings, ffmpegDir, Core.PluginDirIfPresent, finalUrl, tweaks)));
                try
                {
                    p.Start();
                }
                catch (Exception e)
                {
                    RaiseLog($"無法執行 yt-dlp：{e.Message}", "error");
                    break;
                }
                _lastPost.Reset();
                using (ct.Register(() => { _stopping = true; Core.KillTree(p); }))
                {
                    await Task.WhenAll(Pump(p.StandardOutput), Pump(p.StandardError)).ConfigureAwait(false);
                    await p.WaitForExitAsync(CancellationToken.None).ConfigureAwait(false);
                }
                if (ct.IsCancellationRequested) return new(ok, fail, true);
                if (p.ExitCode == 0) { success = true; break; }

                // 失敗：原因 yt-dlp 已經印過了。依原因換個方式再試，沒有辦法就給建議
                string errorText;
                lock (_lineGate) errorText = string.Join("\n", _errors);
                var kind = Core.ClassifyError(errorText);
                if (Core.PlanRetry(kind, tried, ref tweaks, out var message))
                {
                    RaiseLog(message, "warn");
                    if (kind == "network")
                    {
                        try { await Task.Delay(3000, ct).ConfigureAwait(false); }
                        catch (TaskCanceledException) { return new(ok, fail, true); }
                    }
                    continue;
                }
                var usesCookies = Options.ValueOf(Options.Browsers, settings.Browser) != "";
                var hint = Platforms.Hint(Platforms.Of(finalUrl), kind, errorText, usesCookies)      // 這個平台特有的說明優先
                           ?? Core.ErrorHint(kind, settings);
                if (hint is not null) RaiseLog("建議：" + hint, "warn");
                if (Platforms.CaptureIsGoodFallback(Platforms.Of(finalUrl)) && !_captureUrls.Contains(url)) _captureUrls.Add(url);
                break;
            }
            if (success)
            {
                ok++;
                RaiseLog($"完成：{url}", "ok");
            }
            else
            {
                fail++;
                RaiseLog($"失敗：{url}", "error");
            }
        }
        return new(ok, fail, false);
    }

    private async Task Pump(StreamReader reader)
    {
        while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
        {
            lock (_lineGate) HandleLine(line);
        }
    }

    private void HandleLine(string line)
    {
        if (line.Length == 0) return;
        if (line.StartsWith("AVD-DL|", StringComparison.Ordinal))
        {
            HandleDownloadProgress(line.Split('|'));
        }
        else if (line.StartsWith("AVD-PP|", StringComparison.Ordinal))
        {
            var f = line.Split('|');
            if (f.Length > 2 && f[1] == "started")
            {
                var text = Options.PpLabel(f[2]) + "…";
                Raise(() => Status?.Invoke(text));
            }
        }
        else if (_stopping)
        {
            // 程序被結束時的殘餘訊息沒有意義
        }
        else if (line.StartsWith("[debug] ", StringComparison.Ordinal) ||
                 (line.StartsWith("[download]", StringComparison.Ordinal) && line.Contains('%')))
        {
            // 除錯訊息略過；下載百分比已經有進度條
        }
        else if (line.StartsWith("ERROR:", StringComparison.Ordinal))
        {
            _errors.Add(line);
            RaiseLog(line, "error");
        }
        else if (line.StartsWith("WARNING:", StringComparison.Ordinal))
        {
            RaiseLog(line, "warn");
        }
        else
        {
            RaiseLog(line, "info");
        }
    }

    private void HandleDownloadProgress(string[] f)
    {
        // AVD-DL|status|downloaded|total|estimate|speed|eta|frag_index|frag_count|vcodec|acodec|pl_index|n_entries
        string F(int i) => i < f.Length ? f[i] : "";
        var status = F(1);
        if (status == "finished")
        {
            Raise(() => Progress?.Invoke(100.0, "下載完成，後處理中…"));
            return;
        }
        if (status != "downloading") return;
        if (_lastPost.IsRunning && _lastPost.ElapsedMilliseconds < 150) return;  // 節流，避免洗版
        _lastPost.Restart();

        var done = Math.Max(0, Core.Num(F(2)));
        var total = Core.Num(F(3));
        if (total <= 0) total = Core.Num(F(4));
        var pct = total > 0 ? done * 100 / total : -1;
        double fragIndex = Core.Num(F(7)), fragCount = Core.Num(F(8));
        if (pct < 0 && fragCount > 0) pct = Math.Max(0, fragIndex) * 100 / fragCount;

        var part = F(9) == "none" ? "音訊 " : F(10) == "none" ? "影像 " : "";
        var text = $"{part}{Core.FmtBytes(done)} / {Core.FmtBytes(total)}   {Core.FmtBytes(Core.Num(F(5)))}/s   剩餘 {Core.FmtEta(Core.Num(F(6)))}";
        double plIndex = Core.Num(F(11)), nEntries = Core.Num(F(12));
        if (plIndex > 0 && nEntries > 0) text = $"清單 {(long)plIndex}/{(long)nEntries}   " + text;
        Raise(() => Progress?.Invoke(pct, text));
    }
}

/// <summary>只解析、不下載：看標題、長度、可用的畫質。</summary>
public sealed class ProbeJob(string url, Settings settings, Engine engine) : JobBase
{
    public async Task<(bool Ok, string Text)> RunAsync()
    {
        var (finalUrl, note) = await Platforms.ResolveAsync(url).ConfigureAwait(false);
        if (note.Length > 0) RaiseLog(note, "info");
        using var p = Core.CreateProcess(engine.Program, engine.Args(Core.BuildProbeArgs(settings, finalUrl)));
        try
        {
            p.Start();
        }
        catch (Exception e)
        {
            return (false, $"解析失敗：無法執行 yt-dlp（{e.Message}）");
        }
        var stdoutTask = p.StandardOutput.ReadToEndAsync();
        var stderrTask = p.StandardError.ReadToEndAsync();
        await Task.WhenAll(stdoutTask, stderrTask).ConfigureAwait(false);
        await p.WaitForExitAsync().ConfigureAwait(false);

        string? lastError = null;
        foreach (var raw in stderrTask.Result.Split('\n'))
        {
            var l = raw.Trim();
            if (l.StartsWith("WARNING:", StringComparison.Ordinal)) RaiseLog(l, "warn");
            else if (l.StartsWith("ERROR:", StringComparison.Ordinal)) lastError = l;
        }

        JsonObject? info = null;
        try
        {
            info = JsonNode.Parse(stdoutTask.Result) as JsonObject;
        }
        catch (System.Text.Json.JsonException)
        {
        }
        if (p.ExitCode != 0 || info is null)
            return (false, "解析失敗：" + (lastError ?? $"yt-dlp 結束代碼 {p.ExitCode}"));

        string Text(string key) => info[key] is JsonValue v && v.TryGetValue(out string? s) ? s : "";
        double Number(JsonNode? n) => n is JsonValue v && v.TryGetValue(out double d) ? d : 0;

        var lines = new List<string>();
        if (Text("_type") == "playlist")
        {
            var n = (info["entries"] as JsonArray)?.Count ?? 0;
            lines.Add($"播放清單：{(Text("title") is { Length: > 0 } t ? t : "(無標題)")}");
            lines.Add($"共 {n} 項" + (settings.Playlist ? "" : "　—　要全部下載請勾選「下載整個播放清單」"));
        }
        else
        {
            lines.Add($"標題：{(Text("title") is { Length: > 0 } t ? t : "(無標題)")}");
            if (Text("uploader") is { Length: > 0 } up) lines.Add($"上傳者：{up}");
            var duration = Number(info["duration"]);
            if (duration > 0) lines.Add($"長度：{Core.FmtEta(duration)}");
            var heights = (info["formats"] as JsonArray ?? new JsonArray())
                .Select(fmt => (int)Number(fmt?["height"]))
                .Where(h => h > 0).Distinct().OrderDescending().ToList();
            if (heights.Count > 0) lines.Add("可用畫質：" + string.Join("、", heights.Select(h => $"{h}p")));
            var site = Text("extractor_key");
            if (site == "") site = Text("extractor");
            lines.Add($"網站：{(site == "" ? "?" : site)}");
        }
        return (true, string.Join("\n", lines));
    }
}

/// <summary>更新（或安裝）yt-dlp。網站改版時下載核心常需要跟著更新。</summary>
public sealed class UpdateJob(Engine engine) : JobBase
{
    public async Task<int> RunAsync()
    {
        string program;
        List<string> args;
        if (engine.Kind == EngineKind.PyModule)
        {
            program = engine.Program;
            args = ["-3", .. Core.PipInstall];
        }
        else if (engine.Kind == EngineKind.Exe)
        {
            // pip 裝的 yt-dlp.exe 在 <Python>\Scripts 底下，要用 pip 更新；
            // 單一執行檔版（官方下載、winget）用自己的 -U 更新
            var scripts = Path.GetDirectoryName(engine.Program)!;
            var python = Path.GetFullPath(Path.Combine(scripts, "..", "python.exe"));
            if (string.Equals(Path.GetFileName(scripts), "Scripts", StringComparison.OrdinalIgnoreCase) &&
                File.Exists(python))
            {
                program = python;
                args = [.. Core.PipInstall];
            }
            else
            {
                program = engine.Program;
                args = ["-U"];
            }
        }
        else if (Core.FindExecutable("py") is { } py)
        {
            program = py;
            args = ["-3", .. Core.PipInstall];
        }
        else
        {
            RaiseLog("找不到 Python，無法自動安裝。請執行 winget install yt-dlp.yt-dlp，裝完重新開啟本程式。", "error");
            return -1;
        }

        RaiseLog($"> {program} {string.Join(' ', args)}", "info");
        using var p = Core.CreateProcess(program, args);
        try
        {
            p.Start();
        }
        catch (Exception e)
        {
            RaiseLog($"無法執行更新程式：{e.Message}", "error");
            return -1;
        }
        await Task.WhenAll(Pump(p.StandardOutput), Pump(p.StandardError)).ConfigureAwait(false);
        await p.WaitForExitAsync().ConfigureAwait(false);
        return p.ExitCode;
    }

    private async Task Pump(StreamReader reader)
    {
        while (await reader.ReadLineAsync().ConfigureAwait(false) is { } line)
            if (line.Trim().Length > 0) RaiseLog(line.Trim(), "info");
    }
}
