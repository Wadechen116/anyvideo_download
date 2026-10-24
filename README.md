# AnyVideo Download

貼上網址，把網路上的影音下載下來，存成你要的格式。同一套功能與規則，做成三個版本：

| 資料夾 | 技術 | 適合 |
|---|---|---|
| [`py\`](py/README.md) | Python 3 + Tkinter，直接呼叫 yt-dlp 函式庫 | 已經有 Python、想直接改程式 |
| [`qt\`](qt/README.md) | C++17 + Qt 6 Widgets | 要一個原生執行檔，介面反應最快 |
| [`csharp\`](csharp/README.md) | C# + WPF（.NET 8） | Windows 上的 .NET 環境，高 DPI 螢幕顯示最好 |

**功能、格式、取捨規則、疑難排解，三個版本都一樣**，完整說明在
[py\README.md](py/README.md)（「支援的輸出格式」「取捨：畫質優先 vs 相容優先」「其他選項」「疑難排解」）。

---

## 三個版本怎麼做到「規則一樣」

```
                     ┌─────────────────────────────────────────┐
  py\   ─────────────┤ yt-dlp 函式庫（build_ydl_options）       │
                     │                                         ├──> ffmpeg
  qt\     ─┐         │ yt-dlp 命令列                           │
           ├─────────┤   + shared\yt-dlp-plugins（EnsureH264）  │
  csharp\ ─┘         └─────────────────────────────────────────┘
```

- **下載核心都是 yt-dlp，轉檔都是 ffmpeg。** 差別只在怎麼呼叫：Python 版直接用函式庫；
  Qt 版與 C# 版執行 yt-dlp 命令列，一個網址一個程序。
- **格式選擇、排序、合併容器、後處理器的種類與順序**，Qt 版與 C# 版組出來的命令列參數
  經 yt-dlp 自己的解析器還原後，和 Python 版的選項**逐項相同**。`tests\smoke_native.py`
  每次都會檢查這件事（98 組設定）。
- **相容優先的「沒有 H.264 就重新編碼」**：Python 版寫成自訂的後處理器；命令列沒辦法
  塞自訂程式碼，所以同樣的邏輯做成 yt-dlp 外掛放在 `shared\yt-dlp-plugins`，建置時複製到
  Qt 版與 C# 版的執行檔旁邊。
- **cookies.txt**：可指定匯出的 cookies.txt 檔（設定 `cookies_file`），檔案存在時優先於「登入狀態」的瀏覽器 cookies；檔案不存在則忽略。下載與解析都適用，下載前的設定檢查會提醒檔案不見了。
- **設定檔共用**：`%APPDATA%\anyvideo_download\settings.json`，換版本使用時設定沿用。

### Qt 版與 C# 版多做的事

| | Python 版 | Qt 版／C# 版 |
|---|---|---|
| 取消 | 等目前這一步（例如轉檔）結束才停 | **立刻停**：整個 yt-dlp／ffmpeg 程序樹一起結束 |
| 更新下載核心後 | 要重新開啟程式 | 自動重新偵測，不必重開 |
| 啟動時偵測 yt-dlp | 匯入函式庫 | 背景執行 `yt-dlp --version`，視窗不會卡住 |
| 找 yt-dlp 的順序 | Python 環境裡的函式庫 | 程式目錄的 `yt-dlp.exe` → PATH 上的 `yt-dlp.exe` → `py -3 -m yt_dlp` |

「程式目錄的 `yt-dlp.exe`」排第一：把
[官方單一執行檔](https://github.com/yt-dlp/yt-dlp/releases) 放在 `AnyVideoDownload.exe`
旁邊，Qt 版與 C# 版就**完全不需要 Python**。

---

## 三個版本都有的新功能

- **原始品質（不轉檔）**：「取捨」的第三個選項，音訊格式多一個「原始（不轉檔）」。下載網站提供的最高畫質影像與最高音質聲音，
  原封不動封裝（串流複製），與來源解碼後逐格相同。能放進 mp4 才用 mp4，否則 mkv。代價是 AV1／VP9／Opus 在舊裝置上不一定能播。
- **YouTube／抖音（大陸版、國際版 TikTok）／小紅書**：
  - 貼整段分享文字也行：自動找出網址（去掉標題、表情符號、全形標點，補上沒寫的 https://）。
  - 網址改寫成 yt-dlp 認得的形式：抖音的 `?modal_id=`、`iesdouyin.com/share/video/` → `douyin.com/video/<id>`；
    小紅書的用戶頁／搜尋結果頁 → `/explore/<筆記>`（`xsec_token` 一定保留）。
  - 短連結（`v.douyin.com`、`xhslink.com`）自動跟著轉址展開。
  - 失敗時說得出原因：抖音要新的 cookies、小紅書憑證被網路換掉、YouTube 機器人驗證、TikTok 要瀏覽器偽裝元件。
  - 憑證驗證失敗不會當成網路不穩重試。
- **從瀏覽器擷取**：網站頁面解析不了、但瀏覽器能播時（例如 Meta Reels）。開一個 Chrome／Edge（獨立設定檔，登入一次之後會記住），
  你照常播放，程式透過 DevTools 監聽播放器實際載入的影音網址，用 yt-dlp 下載（以 Chrome 身分連線，否則 fbcdn 這類 CDN 會限速），
  再用 ffmpeg 串流複製合併。**自動挑最高畫質與最高音質**：先問頁面裡正在播放的 `<video>` 長度，只留長度最接近的那一組
  （Facebook 會預先載入下一支影片，不能混進來），影像取解析度最高、聲音取位元率最高。抖音／小紅書／TikTok 下載失敗後會詢問要不要改用它。
  限制：只能挑播放器實際請求過的畫質（要更高畫質，先在網站播放器把畫質調高）；有 DRM 的內容抓不到。
  實作：Python 版在 `py\avd_capture.py`（Tk 視窗，需要 `websockets`，隨 `yt-dlp[default]` 安裝）、Qt 版在 `qt\src\capture.cpp`、C# 版在 `csharp\Capture.cs`。

- **下載前的設定防呆**：按「開始下載」前先檢查，只在「確定有問題」時提醒（你刻意的選擇不念）。能修的附修法，修法以畫質最高、不損耗為優先：
  - 致命，不能照原設定做：儲存位置在需要管理員權限的地方（Windows、Program Files、磁碟根目錄…，程式永遠不寫那裡）、儲存位置寫不進去（唯讀、受控資料夾存取）。
  - 提醒：磁碟空間不到 2 GB；「相容優先」卻選了 webm／avi（做不到 H.264，修法：mp4）；勾了嵌入字幕／封面但容器不支援（會悄悄不嵌，修法：mkv，只是重新封裝）；
    原始品質或原始音訊勾了嵌入封面（不會有作用）；wav 嵌封面（修法：flac）。
  - 三個選擇：自動修正並開始、照原設定開始、取消（Python 版是「是／否／取消」）。
  規則在 `py\anyvideo_download.py`、`qt\src\guard.cpp`、`csharp\Guard.cs`，三個版本必須一模一樣。

測試：`tests\guard_flow.py`（防呆規則：同一批 96 個「設定 × 儲存位置狀況」案例，三個版本逐項比對，另有 24 項明確預期）、
`tests\ui_guard.ps1`（Qt、C# 按下「開始下載」時真的跳出防呆對話框、取消就不開始）、`tests\capture_flow.py`（從瀏覽器擷取的離線端對端：本機網頁放一支正在播放的影片，再用 fetch 抓同一支的較高畫質與聲音，以及一支「預先載入的下一支」
影片當干擾，用真的 Chrome／Edge 跑，三個版本都要選對）、`tests\ui_capture.ps1`（Qt、C# 擷取視窗的 UI Automation 測試）、
`tests\platform_urls.py`（網址處理，三個版本同一批 14 項案例）、`tests\original_quality.py`（原始品質：把來源與結果都解碼，
比對每一格畫面與每個聲音取樣的雜湊值，三個版本）、`tests\smoke_native.py`（Qt／C# 參數與 Python 版逐項一致，含新模式）。

## 需要的東西

| 項目 | Python 版 | Qt 版 | C# 版 |
|---|---|---|---|
| yt-dlp | `pip install "yt-dlp[default,curl-cffi]"` | 任一種：pip 安裝、`winget install yt-dlp.yt-dlp`、或 yt-dlp.exe 放程式旁邊 | 同 Qt 版 |
| ffmpeg | `winget install Gyan.FFmpeg` | 同左 | 同左 |
| deno（選配，YouTube 用） | `winget install DenoLand.Deno` | 同左 | 同左 |
| 建置工具 | 不需要 | Qt 6.5（MSVC 2019 64-bit）、Visual Studio 2022、CMake | .NET 8 SDK |
| 執行環境 | Python 3.10+ | 建置時 windeployqt 已把 Qt DLL 放在執行檔旁 | .NET 8 Desktop Runtime |

## 建置與啟動

```
py\run.bat                                  Python 版：直接啟動

qt\build.bat                                Qt 版：建置
qt\build\Release\AnyVideoDownload.exe       Qt 版：啟動

csharp\build.bat                            C# 版：建置
csharp\bin\Release\net8.0-windows\AnyVideoDownload.exe   C# 版：啟動
```

## 測試

```
py -3 py\tests\smoke_test.py                                            Python 版：實際下載 26 項
py -3 tests\smoke_native.py                                             Qt、C#：參數一致性 + 實際下載 26 項
powershell -NoProfile -ExecutionPolicy Bypass -File tests\ui_native.ps1 Qt、C#：介面測試
```

全部**不連任何外部網站**：用 ffmpeg 產生測試影片（H.264、AV1、VP9、DASH、AV1 與 H.264
混合的 DASH），在本機起 HTTP 服務給程式下載，再用 ffprobe 檢查每個輸出檔的格式、編碼、
解析度，並確認沒有留下中間檔。

介面測試會真的開啟視窗，用 Windows UI Automation 檢查設定載入、選項啟用／停用、
背景偵測、關閉時存檔；不會按「開始下載」，設定檔放在暫存資料夾，不動你自己的設定。

## 檔案

```
py\                         Python 版（見 py\README.md）
qt\                         Qt 版（見 qt\README.md）
csharp\                     C# 版（見 csharp\README.md）
shared\yt-dlp-plugins\      EnsureH264 外掛，Qt 版與 C# 版共用
tests\smoke_native.py       Qt、C# 的參數一致性與下載測試
tests\ui_native.ps1         Qt、C# 的介面測試
```
