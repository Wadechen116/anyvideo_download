// AnyVideo Download（C# 版）進入點。
//
// 一般直接開啟視窗。另有給自動測試用的無介面模式，結果印到標準輸出（UTF-8、Tab 分隔），
// 參數與輸出格式和 Qt 版完全相同：
//
//   AnyVideoDownload.exe --headless args     <settings.json> <url>     印出 yt-dlp 參數，一行一個
//   AnyVideoDownload.exe --headless download <settings.json> <url>... 走和視窗相同的下載流程
//   AnyVideoDownload.exe --headless probe    <settings.json> <url>     解析網址
//   AnyVideoDownload.exe --headless urls     <文字檔>                  印出從文字裡找到的每個網址：「平台<Tab>改寫後的網址」
//   AnyVideoDownload.exe --headless guard    <settings.json> [free=位元組] [writable=0|1]  印出下載前防呆找到的問題，與修正後的設定
//   AnyVideoDownload.exe --headless resolve  <網址> [主機...]          展開短連結（額外列出的主機也當短連結，讓測試能用本機伺服器）
//   AnyVideoDownload.exe --headless cookies-detect <資料夾>...         自動偵測 cookies.txt，印出「found<Tab>路徑」，最適合的在前
//   AnyVideoDownload.exe --headless capture  <網址> <輸出資料夾> [等幾秒]  從瀏覽器擷取（開瀏覽器載入網址、等影音出現、下載並合併）
using System.IO;
using System.Text;
using System.Windows;

namespace AnyVideoDownload;

public static class Program
{
    [STAThread]
    public static int Main(string[] args)
    {
        if (args.Length > 0 && args[0] == "--headless")
            return Headless(args).GetAwaiter().GetResult();

        var app = new Application { ShutdownMode = ShutdownMode.OnMainWindowClose };
        return app.Run(new MainWindow());
    }

    private static async Task<int> Headless(string[] args)
    {
        // WinExe 沒有主控台，但呼叫端重導向標準輸出時照樣寫得進去
        using var stdout = new StreamWriter(Console.OpenStandardOutput(), new UTF8Encoding(false))
        {
            AutoFlush = true,
            NewLine = "\n",
        };
        void Out(string line) => stdout.WriteLine(line);

        if (args.Length >= 2 && args[1] == "cookies-detect")   // 只在給定的資料夾找 cookies.txt，一行一個：found<Tab>絕對路徑
        {
            foreach (var p in CookieDetect.Detect(args.Skip(2))) Out("found\t" + p);
            return 0;
        }
        if (args.Length >= 3 && args[1] == "urls")
        {
            foreach (var u in Platforms.ExtractUrls(File.ReadAllText(args[2], Encoding.UTF8)))
            {
                var n = Platforms.NormalizeUrl(u);
                Out(Platforms.Name(Platforms.Of(n)) + "\t" + n);
            }
            return 0;
        }
        if (args.Length >= 3 && args[1] == "guard")
        {
            var gs = Settings.LoadFrom(args[2]);
            long free = -1;
            var writable = true;
            foreach (var a in args.Skip(3))
            {
                if (a.StartsWith("free=")) free = long.Parse(a[5..], System.Globalization.CultureInfo.InvariantCulture);
                else if (a.StartsWith("writable=")) writable = a[9..] != "0";
            }
            var found = DownloadGuard.Check(gs, new DirProbe(writable, free));
            foreach (var i in found)
                Out($"issue\t{i.Id}\t{i.Detail}\t{(i.Fatal ? 1 : 0)}\t{string.Join(';', i.Fix.Select(f => f.Key + "=" + f.Value))}");
            var fixedSettings = DownloadGuard.ApplyFixes(gs, found);
            Out($"fixed\t{fixedSettings.Container}\t{(fixedSettings.EmbedSubs ? 1 : 0)}\t{(fixedSettings.EmbedThumb ? 1 : 0)}\t{fixedSettings.AudioFormat}");
            return 0;
        }
        if (args.Length >= 3 && args[1] == "resolve")
        {
            var (final, note) = await Platforms.ResolveAsync(args[2], args.Skip(3).ToHashSet());
            Out("final\t" + final);
            Out("note\t" + note);
            return 0;
        }
        if (args.Length >= 4 && args[1] == "capture-auto")   // 測自動擷取：成功時印「完成：」並關掉瀏覽器；停在手動則結束代碼 1 且瀏覽器維持開著
        {
            var autoEngine = await Core.DetectEngineAsync();
            if (!autoEngine.Ok) { Out("log\terror\t找不到下載核心 yt-dlp"); return 3; }
            using var session = new CaptureSession();
            session.Status += Out;
            try { session.Start(args[2]); }
            catch (InvalidOperationException e) { Out(e.Message); return 1; }
            var (path, reason, _) = await CaptureRunner.AutoCaptureAsync(session, args[3], autoEngine, Core.FindFfmpegDir(), Out);
            Out("reason\t" + reason);
            if (reason == "ok") { await session.CloseBrowserAsync(); Out("完成：" + path); return 0; }
            return 1;
        }
        if (args.Length >= 4 && args[1] == "capture")
        {
            var captureEngine = await Core.DetectEngineAsync();
            if (!captureEngine.Ok) { Out("log\terror\t找不到下載核心 yt-dlp"); return 3; }
            var wait = args.Length > 4 && double.TryParse(args[4], System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var w) ? w : 4;
            var result = await CaptureRunner.RunHeadlessAsync(args[2], args[3], wait, captureEngine, Core.FindFfmpegDir(), Out);
            return result is null ? 1 : 0;
        }
        if (args.Length < 4)
        {
            Out("usage: --headless args|download|probe <settings.json> <url>...");
            return 2;
        }
        var cmd = args[1];
        var s = Settings.LoadFrom(args[2]);
        var urls = args.Skip(3).ToList();

        if (cmd == "args")
        {
            foreach (var a in Core.BuildDownloadArgs(s, Core.FindFfmpegDir(), Core.PluginDir, urls[0])) Out(a);
            return 0;
        }

        var engine = await Core.DetectEngineAsync();
        if (!engine.Ok)
        {
            Out("log\terror\t找不到下載核心 yt-dlp");
            return 3;
        }

        if (cmd == "probe")
        {
            var job = new ProbeJob(urls[0], s, engine);
            job.Log += (t, tag) => Out($"log\t{tag}\t{t}");
            var (ok, text) = await job.RunAsync();
            Out($"probe\t{(ok ? "ok" : "fail")}\t{text.Replace("\n", " | ")}");
            return ok ? 0 : 1;
        }

        if (cmd == "download")
        {
            var job = new DownloadJob(urls, s, engine, Core.FindFfmpegDir());
            job.Log += (t, tag) => Out($"log\t{tag}\t{t}");
            job.Item += (i, n, url) => Out($"item\t{i}\t{n}\t{url}");
            job.Status += t => Out($"status\t{t}");
            var r = await job.RunAsync(CancellationToken.None);
            Out($"done\t{r.Ok}\t{r.Fail}\t{(r.Cancelled ? 1 : 0)}");
            return r.Fail == 0 && !r.Cancelled ? 0 : 1;
        }

        Out("unknown command: " + cmd);
        return 2;
    }
}
