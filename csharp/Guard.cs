// 下載前的設定防呆：按「開始下載」前先檢查哪些設定其實做不到、會悄悄被略過、或根本寫不進去。
//
// 每一項都是「確定有問題」才提醒（不是你刻意的選擇都要念一次），能修的附上修法，修法以「畫質最高又不損耗」為優先
// （例如改成 mkv：什麼都裝得下，只是重新封裝，不必重新編碼）。
//   致命（不能照原設定做）：dir_admin_only 儲存位置在需要管理員權限的地方、dir_not_writable 儲存位置寫不進去
//   提醒：disk_low 磁碟空間不到 2 GB、compat_not_kept 相容優先卻選了 webm／avi（做不到 H.264）、
//         cookies_missing 指定的 cookies.txt 不存在（會被略過）、
//         subs_dropped／thumb_dropped 勾了嵌入字幕／封面但容器不支援（會悄悄不嵌）
// 規則與 py/anyvideo_download.py、qt/src/guard.cpp 相同（tests/guard_flow.py 三個版本跑同一批案例）。
using System.Globalization;
using System.IO;

namespace AnyVideoDownload;

/// <param name="Fix">（設定名稱, 值）；空＝沒有自動修正</param>
public sealed record GuardIssue(string Id, string Detail, bool Fatal, IReadOnlyList<(string Key, string Value)> Fix);

/// <summary>儲存位置的實際狀況：寫得進去嗎、還剩多少空間（-1＝不知道）。資料夾要已經存在。</summary>
public sealed record DirProbe(bool Writable = true, long Free = -1);

public static class DownloadGuard
{
    public const long MinFreeBytes = 2L * 1024 * 1024 * 1024;

    private static readonly IReadOnlyList<(string, string)> None = [];

    private static string NormPath(string raw)
    {
        var o = raw.Replace('\\', '/').ToLowerInvariant();
        while (o.Length > 1 && o.EndsWith('/')) o = o[..^1];
        // \\?\C:\... 與 \\.\C:\... 是同一個磁碟路徑的另一種寫法
        if (o.Length >= 4 && (o.StartsWith("//?/") || o.StartsWith("//./"))) o = o[4..];
        // WSL 的 /mnt/c/windows 就是 c:/windows
        if (o.Length >= 6 && o.StartsWith("/mnt/") && o[5] is >= 'a' and <= 'z' && (o.Length == 6 || o[6] == '/')) o = o[5] + ":" + o[6..];
        return o;
    }

    private static bool StartsWithFolder(string path, string folder) =>
        path == folder || (path.Length > folder.Length && path.StartsWith(folder, StringComparison.Ordinal) && path[folder.Length] == '/');

    /// <summary>需要管理員權限才能寫的地方（Windows 系統資料夾、Program Files、磁碟根目錄、Unix 的系統資料夾）。程式永遠不寫那裡。</summary>
    public static bool IsAdminOnlyPath(string raw)
    {
        var p = NormPath(raw.Trim());
        if (p.Length == 0) return true;                                     // 沒有路徑不是可以寫的地方
        if (p.Length >= 2 && p[1] == ':' && p[0] is >= 'a' and <= 'z')
        {
            var rest = p.Length > 3 ? p[3..] : "";
            if (rest.Length == 0) return true;                              // 磁碟根目錄本身
            // 直接放在根目錄的檔案；根目錄底下自己建的資料夾（D:/影片）另外判斷
            if (!rest.Contains('/') && rest.Contains('.')) return true;
            foreach (var f in new[] { "windows", "program files", "program files (x86)", "programdata", "$recycle.bin", "system volume information", "recovery" })
                if (StartsWithFolder(rest, f)) return true;
            return false;
        }
        foreach (var f in new[] { "/", "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/opt", "/var", "/boot", "/sys", "/proc", "/dev", "/root", "/srv", "/snap" })
            if (p == f || (f != "/" && StartsWithFolder(p, f))) return true;
        return false;
    }

    public static DirProbe ProbeOutDir(string outDir)
    {
        var writable = true;
        try
        {
            var probe = Path.Combine(outDir, ".avd_write_test_" + Guid.NewGuid().ToString("N")[..8]);
            using (File.Create(probe, 1, FileOptions.DeleteOnClose)) { }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            writable = false;
        }
        long free = -1;
        try
        {
            var root = Path.GetPathRoot(Path.GetFullPath(outDir));
            if (!string.IsNullOrEmpty(root)) free = new DriveInfo(root).AvailableFreeSpace;
        }
        catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException) { }
        return new DirProbe(writable, free);
    }

    public static List<GuardIssue> Check(Settings s, DirProbe probe)
    {
        var issues = new List<GuardIssue>();
        if (IsAdminOnlyPath(s.OutDir)) issues.Add(new("dir_admin_only", s.OutDir, true, None));
        else if (!probe.Writable) issues.Add(new("dir_not_writable", s.OutDir, true, None));
        else if (probe.Free >= 0 && probe.Free < MinFreeBytes)
            issues.Add(new("disk_low", (probe.Free / (1024.0 * 1024.0 * 1024.0)).ToString("F1", CultureInfo.InvariantCulture), false, None));

        // cookies_file 設了但檔案不在：下載會退回瀏覽器設定，多半不是本意。只有「確定不存在」才提醒
        if (s.CookiesFile.Trim() != "" && s.UsableCookiesFile() == "")
            issues.Add(new("cookies_missing", s.CookiesFile, false, [("cookies_file", "")]));

        bool video = s.Mode == "video", original = s.Priority == "original";
        var c = s.Container;
        if (video && s.Priority == "compat" && (c == "webm" || c == "avi"))
            issues.Add(new("compat_not_kept", c, false, [("container", "mp4")]));
        if (video && !original && s.EmbedSubs && !Options.SubsOk.Contains(c))
            issues.Add(new("subs_dropped", c, false, [("container", "mkv")]));
        if (s.EmbedThumb)
        {
            if (video && original) issues.Add(new("thumb_dropped", "original", false, [("embed_thumb", "false")]));
            else if (video && !Options.ThumbOk.Contains(c)) issues.Add(new("thumb_dropped", c, false, [("container", "mkv")]));
            else if (!video && !Options.ThumbOk.Contains(s.AudioFormat))
                issues.Add(new("thumb_dropped", s.AudioFormat, false, s.AudioFormat == "wav" ? [("audio_format", "flac")] : [("embed_thumb", "false")]));
        }
        return issues;
    }

    /// <summary>把每個問題的修法套上去（依序，後面的蓋過前面的）。回傳新的設定，原本的不動。</summary>
    public static Settings ApplyFixes(Settings s, IEnumerable<GuardIssue> issues)
    {
        var o = s.Clone();
        foreach (var i in issues)
            foreach (var (key, value) in i.Fix)
                switch (key)
                {
                    case "container": o.Container = value; break;
                    case "audio_format": o.AudioFormat = value; break;
                    case "embed_thumb": o.EmbedThumb = value == "true"; break;
                    case "embed_subs": o.EmbedSubs = value == "true"; break;
                    case "cookies_file": o.CookiesFile = value; break;
                }
        return o;
    }

    public static string Describe(GuardIssue i)
    {
        var d = i.Detail;
        switch (i.Id)
        {
            case "dir_admin_only":
                return $"儲存位置 {d} 在需要管理員權限的位置（Windows、Program Files 等），程式不會寫入那裡。請選擇你自己的資料夾（例如「文件」或「影片」底下）。";
            case "dir_not_writable":
                return $"無法寫入儲存位置 {d}：資料夾是唯讀、被 Windows「受控資料夾存取」（防勒索軟體）擋住，或權限不足。請換一個位置（例如桌面）。";
            case "disk_low":
                return $"儲存位置所在的磁碟只剩 {d} GB 可用空間。高畫質影片可能好幾 GB，空間不夠時下載到一半會失敗。";
            case "compat_not_kept":
                return $"「相容優先」承諾輸出 H.264 + AAC，但 {d} 容器做不到（webm 裝不下 H.264，avi 一律轉成 MPEG-4 + MP3），結果不會是 H.264。要 H.264 請改用 mp4（畫質最高又能播）。";
            case "cookies_missing":
                return $"已指定 cookies.txt，但找不到這個檔案：{d}。下載時會略過它（改用「登入狀態」的設定）。要清除這個設定嗎？";
            case "subs_dropped":
                return $"已勾選嵌入字幕，但 {d} 容器不支援，字幕不會被嵌入。改用 mkv 可以嵌入（也不必重新編碼）。";
            case "thumb_dropped":
                return d switch
                {
                    "original" => "「原始品質（不轉檔）」要等網站給什麼才知道容器，所以不會嵌入封面；勾選的「嵌入封面」不會有作用。",
                    "best" => "音訊「原始（不轉檔）」不嵌入封面（格式要看網站給什麼才知道）；勾選的「嵌入封面」不會有作用。",
                    "wav" => "wav 不支援嵌入封面。改用 flac（同樣無損）可以嵌入。",
                    _ => $"已勾選嵌入封面，但 {d} 容器不支援，封面不會被嵌入。改用 mkv 可以嵌入。",
                };
            default:
                return i.Id;
        }
    }
}
