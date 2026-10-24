// 自動偵測電腦上匯出的 cookies.txt（Netscape 格式）。規則與 py/、qt/ 版相同。
using System.IO;
using System.Text;

namespace AnyVideoDownload;

public static class CookieDetect
{
    public const long MaxBytes = 5L * 1024 * 1024;

    public static List<string> DefaultDirs(string? outDir)
    {
        var dirs = new List<string>();
        var home = Environment.GetEnvironmentVariable("USERPROFILE");
        if (!string.IsNullOrEmpty(home))
            foreach (var n in new[] { "Downloads", "Desktop", "Documents" }) dirs.Add(Path.Combine(home, n));
        var appData = Environment.GetEnvironmentVariable("APPDATA");
        if (!string.IsNullOrEmpty(appData)) dirs.Add(Path.Combine(appData, "anyvideo_download"));
        if (!string.IsNullOrWhiteSpace(outDir)) dirs.Add(outDir.Trim());
        dirs.Add(AppContext.BaseDirectory);
        return dirs;
    }

    private static string Decode(byte[] b, int count)
    {
        var s = new UTF8Encoding(false, false).GetString(b, 0, count);   // 寬鬆：壞位元組變成 U+FFFD
        return s.Length > 0 && s[0] == '\uFEFF' ? s[1..] : s;
    }

    private static bool IsCookieLine(string line)
    {
        line = line.TrimEnd('\r');
        if (line.Trim().Length == 0) return false;
        if (line.StartsWith("#HttpOnly_", StringComparison.Ordinal)) line = line[10..];
        if (line.StartsWith('#')) return false;
        return line.Split('\t').Length == 7;
    }

    private static bool IsValid(string text, byte[] raw, int count)
    {
        var head = Decode(raw, Math.Min(count, 4096));
        if (head.Contains("Netscape HTTP Cookie File", StringComparison.Ordinal) ||
            head.Contains("HTTP Cookie File", StringComparison.Ordinal)) return true;
        foreach (var line in text.Split('\n'))
            if (IsCookieLine(line)) return true;
        return false;
    }

    public static bool HasDouyinTiktok(string path)
    {
        try
        {
            var raw = File.ReadAllBytes(path);
            var t = Decode(raw, (int)Math.Min(raw.Length, MaxBytes));
            return t.Contains("douyin.com", StringComparison.Ordinal) || t.Contains("tiktok.com", StringComparison.Ordinal);
        }
        catch (Exception) { return false; }
    }

    public static List<string> Detect(IEnumerable<string> dirs)
    {
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var items = new List<(string Path, bool Dy, bool Name, DateTime Mtime)>();
        foreach (var d in dirs)
        {
            if (string.IsNullOrWhiteSpace(d)) continue;
            string[] files;
            try
            {
                var full = Path.GetFullPath(d);
                if (!Directory.Exists(full)) continue;
                files = Directory.GetFiles(full, "*.txt", SearchOption.TopDirectoryOnly);
            }
            catch (Exception) { continue; }
            foreach (var f0 in files)
            {
                try
                {
                    var f = Path.GetFullPath(f0).Replace('/', '\\');
                    if (!f.EndsWith(".txt", StringComparison.OrdinalIgnoreCase)) continue;
                    var info = new FileInfo(f);
                    if (!info.Exists || info.Length > MaxBytes) continue;
                    if (seen.Contains(f)) continue;
                    var raw = File.ReadAllBytes(f);
                    var text = Decode(raw, raw.Length);
                    if (!IsValid(text, raw, raw.Length)) continue;
                    seen.Add(f);
                    var dy = text.Contains("douyin.com", StringComparison.Ordinal) || text.Contains("tiktok.com", StringComparison.Ordinal);
                    var nm = Path.GetFileName(f).Contains("cookie", StringComparison.OrdinalIgnoreCase);
                    items.Add((f, dy, nm, info.LastWriteTimeUtc));
                }
                catch (Exception) { }
            }
        }
        return items
            .OrderByDescending(i => i.Dy).ThenByDescending(i => i.Name).ThenByDescending(i => i.Mtime)
            .ThenBy(i => i.Path, StringComparer.Ordinal)
            .Select(i => i.Path).ToList();
    }
}
