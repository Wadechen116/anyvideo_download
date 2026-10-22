# AnyVideo Download — C# 版

C# 12 + WPF（.NET 8）。功能、格式與取捨規則和 Python 版完全相同，使用說明見
[py\README.md](../py/README.md)；三個版本的差異見 [上一層的 README](../README.md)。

## 建置

需要：.NET 8 SDK 以上（`dotnet --list-sdks` 確認）。

```
csharp\build.bat
```

或 `dotnet build -c Release`，也可以用 Visual Studio 2022 直接開 `AnyVideoDownload.csproj`。

產出：`bin\Release\net8.0-windows\AnyVideoDownload.exe`，旁邊會有從 `..\shared` 複製來的
`yt-dlp-plugins`。執行需要 [.NET 8 Desktop Runtime](https://dotnet.microsoft.com/download/dotnet/8.0)。

想要不必另裝 .NET 的單一執行檔：

```
dotnet publish -c Release -r win-x64 --self-contained -p:PublishSingleFile=true
```

## 為什麼用 WPF 而不是 WinForms

- **高 DPI**：WPF 以向量繪製，搭配 `app.manifest` 的 PerMonitorV2，125%／150% 縮放或拖到
  不同縮放比例的螢幕都清楚；WinForms 在混合 DPI 下常有版面錯位。
- **版面**：Grid／DockPanel 自動依內容與字型調整大小，不必手動算座標。
- **非同步**：`async`／`await` 等待 yt-dlp 程序，事件自動回到介面執行緒，不需要自己寫
  佇列與輪詢。

## 程式結構

```
Options.cs              格式選項清單、設定檔讀寫（與 Python 版同格式）
Engine.cs               下載核心：找 yt-dlp、組命令列參數、DownloadJob／ProbeJob／UpdateJob
MainWindow.xaml(.cs)    視窗
Program.cs              進入點；另有 --headless 模式給自動測試
app.manifest            PerMonitorV2 DPI 設定
```

**與介面無關的部分都在 `Engine.cs`**，規則對應 Python 版的 `build_ydl_options()`。

- 參數用 `ProcessStartInfo.ArgumentList` 逐一傳遞，路徑有空白、中文都不必自己處理引號。
- yt-dlp 的進度用 `--progress-template` 改成一行一筆、`|` 分隔的格式（`AVD-DL|…`、
  `AVD-PP|…`），不去解析給人看的進度文字。
- 背景工作的事件由 `JobBase` 送回建立它的執行緒（`SynchronizationContext`）；無介面模式
  沒有同步內容時就地觸發。
- 取消時 `Process.Kill(entireProcessTree: true)`，連 yt-dlp 開出來的 ffmpeg 一起停。

## 無介面模式（測試用）

```
AnyVideoDownload.exe --headless args     <settings.json> <url>      印出 yt-dlp 參數
AnyVideoDownload.exe --headless download <settings.json> <url>...  走和視窗相同的下載流程
AnyVideoDownload.exe --headless probe    <settings.json> <url>      解析網址
```

結果以 UTF-8、Tab 分隔印到標準輸出，格式與 Qt 版相同。`tests\smoke_native.py` 用的就是這個。
