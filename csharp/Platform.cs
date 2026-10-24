// 影音平台的網址處理：從分享文字裡找出網址、辨認平台、改寫成 yt-dlp 認得的形式、解開短連結。
//
// 為什麼需要：使用者貼的常常不是乾淨的網址。抖音與小紅書的「分享」會複製一整段文字（標題、表情符號、
// 「復制打開抖音」之類的話，中間夾著 https://v.douyin.com/xxxx/ 這樣的短連結）；瀏覽器網址列的抖音網址常是
// douyin.com/jingxuan?modal_id=123，小紅書常是 xiaohongshu.com/user/profile/<用戶>/<筆記>。yt-dlp 的解析器只認
// douyin.com/video/<id> 與 xiaohongshu.com/explore/<id>，其他形式要先改寫，否則一律「Unsupported URL」。
// 規則與 py/anyvideo_download.py、qt/src/platform.cpp 相同（tests/platform_urls.py 三個版本跑同一批案例）。
using System.Net.Http;
using System.Text.RegularExpressions;

namespace AnyVideoDownload;

public enum PlatformKind { Other, YouTube, Douyin, TikTok, XiaoHongShu }

public static partial class Platforms
{
    // 這些字元不會是網址的一部分，貼上的分享文字裡它們把網址和下一句話隔開
    private const string StopChars = "\\s<>\"'`\u3000，。！？、；：（）【】「」『』《》〈〉…“”‘’";

    private static readonly Regex FullUrl = new("https?://[^" + StopChars + "]+", RegexOptions.IgnoreCase);

    // 分享文字常省略 https://：只認得這幾個已知網域，避免把一般文字當成網址
    private static readonly Regex BareUrl = new(
        @"(?<![/\w.])((?:v\.douyin\.com|xhslink\.com|vm\.tiktok\.com|vt\.tiktok\.com|www\.xiaohongshu\.com|www\.douyin\.com|www\.tiktok\.com|youtu\.be)/[^" + StopChars + "]+)",
        RegexOptions.IgnoreCase);

    [GeneratedRegex(@"/(?:video|share/video|note|share/note)/(\d+)")]
    private static partial Regex DouyinPathId();

    [GeneratedRegex(@"/(?:user/profile/[0-9a-f]+|search_result|explore|discovery/item)/([0-9a-f]{16,})")]
    private static partial Regex XhsNote();

    private const string MobileUa =
        "Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1";

    private static bool HostIs(string host, string domain) => host == domain || host.EndsWith("." + domain);

    private static string HostOf(string url) =>
        Uri.TryCreate(url, UriKind.Absolute, out var u) ? u.Host.ToLowerInvariant() : "";

    public static string Name(PlatformKind p) => p switch
    {
        PlatformKind.YouTube => "YouTube",
        PlatformKind.Douyin => "抖音",
        PlatformKind.TikTok => "TikTok",
        PlatformKind.XiaoHongShu => "小紅書",
        _ => "",
    };

    public static PlatformKind Of(string url)
    {
        var h = HostOf(url);
        if (HostIs(h, "youtube.com") || HostIs(h, "youtu.be") || HostIs(h, "youtube-nocookie.com")) return PlatformKind.YouTube;
        if (HostIs(h, "douyin.com") || HostIs(h, "iesdouyin.com") || HostIs(h, "douyinvod.com")) return PlatformKind.Douyin;
        if (HostIs(h, "tiktok.com")) return PlatformKind.TikTok;
        if (HostIs(h, "xiaohongshu.com") || HostIs(h, "xhslink.com") || HostIs(h, "xhscdn.com")) return PlatformKind.XiaoHongShu;
        return PlatformKind.Other;
    }

    /// <summary>一段文字裡所有的網址（依出現順序、不重複）。去掉網址後面黏著的標點（含全形的 。，！？）、「」等。</summary>
    public static List<string> ExtractUrls(string text)
    {
        var hits = new List<(int Pos, string Url)>();
        foreach (Match m in FullUrl.Matches(text)) hits.Add((m.Index, m.Value));
        foreach (Match m in BareUrl.Matches(text))
        {
            var start = m.Groups[1].Index;
            if (!hits.Any(h => start >= h.Pos && start < h.Pos + h.Url.Length))        // 已經被有 https:// 的涵蓋就不重複算
                hits.Add((start, "https://" + m.Groups[1].Value));
        }
        var result = new List<string>();
        foreach (var (_, raw) in hits.OrderBy(h => h.Pos))
        {
            var u = raw.TrimEnd('.', ',', ';', ':', '!', '?', ')', ']', '}', '>', '*');   // 句尾的標點不是網址
            if (HostOf(u).Length > 0 && !result.Contains(u)) result.Add(u);
        }
        return result;
    }

    /// <summary>抖音網址裡的影片 id（douyin.com/video/ID、?modal_id=ID、iesdouyin.com/share/video/ID）；沒有回傳空字串。</summary>
    public static string DouyinVideoId(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u)) return "";
        var h = u.Host.ToLowerInvariant();
        if (!HostIs(h, "douyin.com") && !HostIs(h, "iesdouyin.com")) return "";
        if (DouyinPathId().Match(u.AbsolutePath) is { Success: true } m) return m.Groups[1].Value;
        foreach (var part in u.Query.TrimStart('?').Split('&', StringSplitOptions.RemoveEmptyEntries))      // 精選、推薦、用戶頁裡點開的影片
        {
            var kv = part.Split('=', 2);
            if (kv[0] == "modal_id" && kv.Length == 2 && kv[1].Length > 0 && kv[1].All(char.IsAsciiDigit)) return kv[1];
        }
        return "";
    }

    /// <summary>改寫成 yt-dlp 認得的形式；不需要改寫的原樣回傳。小紅書網址裡的 xsec_token 等參數必須保留（沒有它網站不給內容）。</summary>
    public static string NormalizeUrl(string raw)
    {
        var url = raw.Trim();
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u)) return url;
        var host = u.Host.ToLowerInvariant();
        switch (Of(url))
        {
            case PlatformKind.Douyin:
                if (u.AbsolutePath.Contains("/note/")) return url;                  // 圖文：yt-dlp 沒有解析器，不改寫
                var id = DouyinVideoId(url);
                return id.Length > 0 ? "https://www.douyin.com/video/" + id : url;
            case PlatformKind.XiaoHongShu when HostIs(host, "xiaohongshu.com"):
                // /user/profile/<用戶>/<筆記>、/search_result/<筆記>、/explore/<筆記> → /explore/<筆記>；查詢參數原樣保留
                if (XhsNote().Match(u.AbsolutePath) is { Success: true } m)
                    return "https://www.xiaohongshu.com/explore/" + m.Groups[1].Value + u.Query;
                return url;
            default:
                return url;
        }
    }

    /// <summary>短連結：要先跟著轉址走，才知道真正的影片網址（v.douyin.com、xhslink.com）。</summary>
    public static bool IsShortLink(string url, IReadOnlySet<string>? extraHosts = null)
    {
        var h = HostOf(url);
        return (extraHosts?.Contains(h) ?? false) || h == "v.douyin.com" || HostIs(h, "xhslink.com");
    }

    /// <summary>
    /// 把貼上的網址變成可以交給 yt-dlp 的網址：先改寫，是短連結就跟著轉址走到真正的網址再改寫一次。
    /// 轉址用手機瀏覽器的身分（分享連結是給手機開的，電腦版身分常被導去登入頁）。
    /// 回傳（最終網址, 說明）：說明是給紀錄區看的一行（沒有話說時空字串）；展開失敗時最終網址是原來的，說明講原因。
    /// </summary>
    public static async Task<(string Final, string Note)> ResolveAsync(
        string url, IReadOnlySet<string>? extraHosts = null, CancellationToken ct = default)
    {
        var original = url;
        var current = NormalizeUrl(url);
        using var http = new HttpClient(new HttpClientHandler { AllowAutoRedirect = false }) { Timeout = TimeSpan.FromSeconds(15) };   // 自己一步一步走
        http.DefaultRequestHeaders.UserAgent.ParseAdd(MobileUa);
        for (var hops = 0; ; hops++)
        {
            if (!IsShortLink(current, extraHosts))
            {
                var p = Of(current);
                return (current, current != original && p != PlatformKind.Other ? $"辨識為{Name(p)}：{current}" : "");
            }
            if (hops >= 6) return (original, $"短連結轉址超過 6 次，放棄展開：{original}");
            string? location;
            string why = "沒有轉址";
            try
            {
                using var resp = await http.GetAsync(current, HttpCompletionOption.ResponseHeadersRead, ct).ConfigureAwait(false);
                location = resp.Headers.Location?.OriginalString;
                if (location is null) why = $"HTTP {(int)resp.StatusCode}";
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException)
            {
                return (original, $"短連結展開失敗（{e.Message}）：{original}");
            }
            if (string.IsNullOrWhiteSpace(location)) return (original, $"短連結展開失敗（{why}）：{original}");
            current = NormalizeUrl(new Uri(new Uri(current), location.Trim()).AbsoluteUri);
        }
    }

    /// <summary>這個平台的下載失敗後，改用「從瀏覽器擷取」常常有效（yt-dlp 的解析器常因網站改版而壞）。</summary>
    public static bool CaptureIsGoodFallback(PlatformKind p) => p is PlatformKind.Douyin or PlatformKind.XiaoHongShu or PlatformKind.TikTok;

    /// <summary>這個平台下載失敗時，使用者該知道的事。kind 是 Core.ClassifyError 的分類；沒有特別的話回傳 null。</summary>
    public static string? Hint(PlatformKind p, string kind, string errorText, bool usesCookies, bool cookiesFile = false)
    {
        switch (p)
        {
            case PlatformKind.Douyin:
                if (errorText.Contains("fresh cookies", StringComparison.OrdinalIgnoreCase) || kind == "login")
                {
                    if (cookiesFile)
                        return "抖音要求「新的 cookies」，但你指定的 cookies.txt 可能已過期，或不是從 douyin.com 匯出的：請先用瀏覽器開 douyin.com、隨便看一支影片，再重新匯出 cookies.txt 並重新選取。"
                               + "還是不行就用「從瀏覽器擷取」：瀏覽器能播，就能抓。";
                    return "抖音要求「新的 cookies」（不一定要登入）：先用 Firefox 或 Chrome 開 douyin.com、隨便看一支影片讓網站發給瀏覽器 cookies，再到「登入狀態」選那個瀏覽器。"
                           + (usesCookies ? "已經選了瀏覽器仍失敗：請先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies），或改用 Firefox。" : "")
                           + "還是不行就用「從瀏覽器擷取」：瀏覽器能播，就能抓。";
                }
                if (kind == "update" || errorText.Contains("Unsupported URL"))
                    return "這個抖音網址形式 yt-dlp 不支援（例如圖文 /note/）或網站改版了。可以改用「從瀏覽器擷取」。";
                break;
            case PlatformKind.XiaoHongShu:
                if (errorText.Contains("certificate", StringComparison.OrdinalIgnoreCase) || errorText.Contains("SSL"))
                    return "連到小紅書時憑證驗證失敗：你目前的網路把 xiaohongshu.com 導向了別的伺服器（憑證不是小紅書的），所以不是影片或程式的問題。這是網路層的限制，程式沒辦法處理；在能正常連到小紅書的網路環境下就可以下載。";
                if (kind is "login" or "update" || errorText.Contains("Unsupported URL"))
                    return "小紅書的網址要帶 xsec_token 參數才給內容：請貼「分享」複製出來的完整網址（含 ?xsec_token=…），不要自己刪掉後面的參數。短連結 xhslink.com 程式會自己展開。仍不行可以改用「從瀏覽器擷取」。";
                break;
            case PlatformKind.TikTok when kind is "impersonate" or "update":
                return "TikTok 常擋非瀏覽器的連線：按「更新下載核心」安裝瀏覽器偽裝元件（curl_cffi）；仍不行可以改用「從瀏覽器擷取」。";
            case PlatformKind.YouTube when kind is "bot" or "login":
                return "YouTube 要求證明不是機器人：「登入狀態」選一個已登入 YouTube 的瀏覽器（Firefox 最可靠）。另外請確認已安裝 deno（winget install DenoLand.Deno），YouTube 需要它才能取得完整格式。";
        }
        return null;
    }
}
