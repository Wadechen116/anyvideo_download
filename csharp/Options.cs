// 格式選項與使用者設定。
//
// 選項清單與 py/anyvideo_download.py、qt/src/settings.h 三邊一致；設定檔也是同一個
// （%APPDATA%\anyvideo_download\settings.json），換版本使用時設定會沿用。
using System.IO;
using System.Text.Encodings.Web;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace AnyVideoDownload;

/// <summary>顯示文字 + 存進設定檔的值。設定檔存值而不存文字，以後改顯示文字時舊設定才不會失效。</summary>
public sealed record Choice(string Label, string Value);

public static class Options
{
    public const string AppName = "AnyVideo Download";
    public const string AppVersion = "1.0.0";

    public static readonly string[] VideoContainers = ["mp4", "mkv", "webm", "mov", "avi"];

    // Value 空字串 = 不限制高度
    public static readonly Choice[] VideoQualities =
    [
        new("最佳", ""), new("2160p", "2160"), new("1440p", "1440"), new("1080p", "1080"),
        new("720p", "720"), new("480p", "480"), new("360p", "360"),
    ];

    public static readonly Choice[] Priorities =
    [
        new("畫質優先 — 最高解析度，可能是 AV1／VP9", "quality"),
        new("相容優先 — H.264，到哪都能播", "compat"),
        // 不轉檔：網站提供的最高畫質影像與最高音質聲音，原封不動封裝（串流複製）。能放進 mp4 才用 mp4，否則 mkv。
        // 與來源逐格相同，沒有任何損耗；代價是 AV1／VP9／Opus 在舊電視、舊手機、部分剪輯軟體上不一定能播。
        new("原始品質 — 不轉檔，無損耗（常是 mkv）", "original"),
    ];

    // 相容優先時保證輸出 H.264 的容器。webm 規格裝不下 H.264，avi 另有固定編碼。
    public static readonly HashSet<string> CompatH264Containers = ["mp4", "mkv", "mov"];

    public static readonly string[] AudioFormats = ["mp3", "m4a", "opus", "ogg", "flac", "wav"];

    // 音訊的「原始格式」：不轉檔，存成網站原本的聲音（opus、m4a…）。設定檔存 "best"（yt-dlp --audio-format 的寫法）。
    public const string AudioOriginalLabel = "原始（不轉檔）";
    public const string AudioOriginalValue = "best";
    public static readonly string[] AudioBitrates = ["320", "256", "192", "128", "96"];
    public static readonly HashSet<string> LosslessAudio = ["flac", "wav"];

    // 能嵌入封面／字幕的容器。不支援時直接停用勾選框，而不是讓使用者勾了才失敗。
    public static readonly HashSet<string> ThumbOk = ["mp4", "mkv", "mov", "mp3", "m4a", "opus", "ogg", "flac"];
    public static readonly HashSet<string> SubsOk = ["mp4", "mkv", "webm", "mov"];

    // Value 空字串 = 不使用
    public static readonly Choice[] Browsers =
    [
        new("不使用", ""), new("Firefox", "firefox"), new("Edge", "edge"), new("Chrome", "chrome"),
    ];

    private static readonly Dictionary<string, string> PpLabels = new()
    {
        ["Merger"] = "合併影像與聲音",
        ["VideoConvertor"] = "轉換影片格式",
        ["VideoRemuxer"] = "重新封裝",
        ["ExtractAudio"] = "轉出音訊",
        ["Metadata"] = "寫入標題等資訊",
        ["EmbedThumbnail"] = "嵌入封面",
        ["EmbedSubtitle"] = "嵌入字幕",
        ["MoveFiles"] = "整理檔案",
        ["EnsureH264"] = "轉成相容編碼 H.264（比較慢）",
    };

    /// <summary>後處理器名稱 -> 狀態列文字</summary>
    public static string PpLabel(string name) => PpLabels.GetValueOrDefault(name, name);

    public static string ValueOf(IEnumerable<Choice> choices, string label, string fallback = "") =>
        choices.FirstOrDefault(c => c.Label == label)?.Value ?? fallback;

    public static string LabelOf(IEnumerable<Choice> choices, string value, string fallback = "") =>
        choices.FirstOrDefault(c => c.Value == value)?.Label ?? fallback;

    public static bool HasLabel(IEnumerable<Choice> choices, string label) => choices.Any(c => c.Label == label);
}

public sealed class Settings
{
    public Settings Clone() => (Settings)MemberwiseClone();

    public string OutDir { get; set; } =
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Downloads");
    public string Mode { get; set; } = "video";        // "video" 或 "audio"
    public string Container { get; set; } = "mp4";
    public string Quality { get; set; } = "最佳";
    public string Priority { get; set; } = "quality";  // "quality" 或 "compat"
    public string AudioFormat { get; set; } = "mp3";
    public string AudioBitrate { get; set; } = "192";
    public bool Playlist { get; set; }
    public bool EmbedMeta { get; set; } = true;
    public bool EmbedThumb { get; set; }
    public bool EmbedSubs { get; set; }
    public string Browser { get; set; } = "不使用";
    public string CookiesFile { get; set; } = "";      // cookies.txt 路徑；存在時優先於 Browser

    /// <summary>可用的 cookies.txt 路徑（設了而且檔案存在）；否則 ""（檔案不見就當沒設，退回瀏覽器設定）。</summary>
    public string UsableCookiesFile()
    {
        var p = (CookiesFile ?? "").Trim();
        try { return p != "" && File.Exists(p) ? p : ""; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException) { return ""; }
    }

    public static string SettingsPath
    {
        get
        {
            var baseDir = Environment.GetEnvironmentVariable("APPDATA");
            if (string.IsNullOrEmpty(baseDir)) baseDir = Environment.GetFolderPath(Environment.SpecialFolder.UserProfile);
            return Path.Combine(baseDir, "anyvideo_download", "settings.json");
        }
    }

    public static Settings Load() => LoadFrom(SettingsPath);

    /// <summary>讀不到、格式不對的欄位就用預設值。</summary>
    public static Settings LoadFrom(string path)
    {
        var s = new Settings();
        JsonObject? o;
        try
        {
            o = JsonNode.Parse(File.ReadAllText(path)) as JsonObject;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException)
        {
            return s;
        }
        if (o is null) return s;

        string Str(string key, string current) =>
            o[key] is JsonValue v && v.TryGetValue(out string? x) ? x : current;
        bool Flag(string key, bool current) =>
            o[key] is JsonValue v && v.TryGetValue(out bool x) ? x : current;

        s.OutDir = Str("out_dir", s.OutDir);
        s.Mode = Str("mode", s.Mode);
        s.Container = Str("container", s.Container);
        s.Quality = Str("quality", s.Quality);
        s.Priority = Str("priority", s.Priority);
        s.AudioFormat = Str("audio_format", s.AudioFormat);
        s.AudioBitrate = Str("audio_bitrate", s.AudioBitrate);
        s.Playlist = Flag("playlist", s.Playlist);
        s.EmbedMeta = Flag("embed_meta", s.EmbedMeta);
        s.EmbedThumb = Flag("embed_thumb", s.EmbedThumb);
        s.EmbedSubs = Flag("embed_subs", s.EmbedSubs);
        s.Browser = Str("browser", s.Browser);
        s.CookiesFile = Str("cookies_file", s.CookiesFile);
        return s;
    }

    public void Save()
    {
        // 鍵名與 Python 版相同（snake_case），三個版本共用這個檔
        var o = new JsonObject
        {
            ["out_dir"] = OutDir, ["mode"] = Mode, ["container"] = Container, ["quality"] = Quality,
            ["priority"] = Priority, ["audio_format"] = AudioFormat, ["audio_bitrate"] = AudioBitrate,
            ["playlist"] = Playlist, ["embed_meta"] = EmbedMeta, ["embed_thumb"] = EmbedThumb,
            ["embed_subs"] = EmbedSubs, ["browser"] = Browser, ["cookies_file"] = CookiesFile,
        };
        try
        {
            var path = SettingsPath;
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            var json = o.ToJsonString(new JsonSerializerOptions
            {
                WriteIndented = true,
                Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,  // 中文直接寫，不要 \uXXXX
            });
            File.WriteAllText(path, json);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            // 存不了設定不該讓下載失敗
        }
    }
}
