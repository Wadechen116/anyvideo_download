# AnyVideo Download — Qt 版

C++17 + Qt 6 Widgets。功能、格式與取捨規則和 Python 版完全相同，使用說明見
[py\README.md](../py/README.md)；三個版本的差異見 [上一層的 README](../README.md)。

## 建置

需要：Qt 6.5 以上（MSVC 2019 64-bit 套件）、Visual Studio 2022（C++ 桌面開發）、CMake 3.21 以上。

```
qt\build.bat
```

預設使用 `C:\Qt\6.5.1\msvc2019_64`，其他版本先設定環境變數：

```
set QTDIR=C:\Qt\6.8.0\msvc2022_64
qt\build.bat
```

`build.bat` 做三件事：

1. CMake 產生 Visual Studio 專案並以 Release 建置
2. 把 `..\shared\yt-dlp-plugins` 複製到執行檔旁邊（CMake 的 POST_BUILD）
3. `windeployqt` 把 Qt DLL 與外掛放到執行檔旁邊

產出：`build\Release\AnyVideoDownload.exe`。整個 `build\Release` 資料夾可以直接複製到別台電腦
（另需 [VC++ 可轉散發套件](https://aka.ms/vs/17/release/vc_redist.x64.exe)、yt-dlp、ffmpeg）。

也可以用 Qt Creator 直接開 `CMakeLists.txt`。

## 程式結構

```
src\settings.h / .cpp     格式選項清單、設定檔讀寫（與 Python 版同格式）
src\engine.h / .cpp       下載核心：找 yt-dlp、組命令列參數、DownloadJob／ProbeJob／UpdateJob
src\mainwindow.h / .cpp   視窗
src\main.cpp              進入點；另有 --headless 模式給自動測試
```

**與介面無關的部分都在 `engine.cpp`**，規則對應 Python 版的 `build_ydl_options()`。

- 子程序用 `QProcess` 的非同步訊號處理，不另開執行緒；只有啟動時的 `yt-dlp --version`
  偵測放在背景執行緒。
- yt-dlp 的進度用 `--progress-template` 改成一行一筆、`|` 分隔的格式（`AVD-DL|…`、
  `AVD-PP|…`），不去解析給人看的進度文字，yt-dlp 改版也不會壞。
- 取消時用 `taskkill /T` 結束整棵程序樹，連 yt-dlp 開出來的 ffmpeg 一起停。

## Qt 版獨有：原始品質、從瀏覽器擷取

### 「取捨」的第三個選項：原始品質（不轉檔）

下載到網站提供的**最高畫質影像與最高音質聲音**，原封不動封裝（串流複製），不重新編碼：

- 格式選擇用 yt-dlp 自己的排序（解析度、幀率、HDR、編碼、位元率），聲音挑位元率最高的；不套用 `ext:mp4:m4a`
  這類偏好相容容器的排序（它可能挑到位元率較低的 AAC，而不是最高音質的 Opus）。
- 容器由來源決定：影像與聲音都放得進 mp4 就用 mp4，否則 mkv（什麼都裝得下）。「影片格式」欄位在這個模式下停用。
- 不轉檔、不重新封裝成別的格式、不跑 EnsureH264。
- 「音訊格式」多一個「原始（不轉檔）」：聲音存成網站原本的格式（m4a、opus…），沒有二次壓縮。
- 代價：AV1／VP9／Opus 在舊電視、舊手機、部分剪輯軟體上不一定能播。要「到處都能播」請用相容優先。
- 能得到的上限就是網站實際提供的；需要登入才有的高畫質，要在「登入狀態」選已登入的瀏覽器。

`tests\original_quality.py` 驗證這個承諾：把來源與結果都解碼，比對每一格畫面與每個聲音取樣的雜湊值，
相同才算沒有損耗（H.264、AV1、VP9、影像聲音分開的 DASH、混合 DASH、音訊原始格式，另有 mp3 當有損的對照）。

### 從瀏覽器擷取

網站頁面解析不了、但瀏覽器能播時（例如 Meta Reels）：開一個 Chrome／Edge（獨立設定檔，登入一次之後會記住），
你照常播放，程式透過 DevTools 監聽播放器實際載入的影音網址，交給 yt-dlp 下載（以 Chrome 身分連線，否則 fbcdn 這類
CDN 會限速），再用 ffmpeg 串流複製合併。**自動挑最高畫質與最高音質**：

1. 先問頁面裡正在播放的 `<video>`：長度幾秒。Facebook 會預先載入下一支影片，要靠長度把它們分開。
2. 把偵測到的檔案（最多 12 個）全部下載，用 ffprobe 量每個檔案的解析度、位元率、長度。
3. 只留長度和正在播放的影片最接近的那一組（差距容許 0.15 秒，不是 ±0.6 秒：實測 10.4 秒的影片旁邊有 10.0 秒的另一支）。
4. 影像取解析度最大的（同解析度取位元率最高），聲音取位元率最高的，合併成 mp4。

限制：只能挑「播放器實際請求過的」畫質。播放器的自動畫質切換一開始常常先請求低畫質；要更高畫質，先在網站播放器裡把畫質
調高（例如 Facebook 的 HD）再擷取。有 DRM 的內容抓不到。

## 無介面模式（測試用）

```
AnyVideoDownload.exe --headless args     <settings.json> <url>      印出 yt-dlp 參數
AnyVideoDownload.exe --headless download <settings.json> <url>...  走和視窗相同的下載流程
AnyVideoDownload.exe --headless probe    <settings.json> <url>      解析網址
```

結果以 UTF-8、Tab 分隔印到標準輸出，格式與 C# 版相同。`tests\smoke_native.py` 用的就是這個。
