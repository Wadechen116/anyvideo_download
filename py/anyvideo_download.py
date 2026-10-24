"""AnyVideo Download —— 貼上網址，下載網路影音並存成需要的格式。

下載核心是 yt-dlp（支援上千個網站），格式合併與轉換靠 ffmpeg。
介面用 Tkinter，隨 Python 內建，不需要另外安裝 GUI 套件。

介面與下載分在兩個執行緒:Tkinter 不是執行緒安全的，所以下載執行緒不直接碰
任何元件，只把訊息丟進佇列，由主執行緒每 100 ms 取出來更新畫面。
"""
from __future__ import annotations

import ctypes
import json
import os
import queue
import shutil
import subprocess
import sys
import threading
import time
from dataclasses import asdict, dataclass, fields
from datetime import datetime
from pathlib import Path

import tkinter as tk
from tkinter import filedialog, messagebox, ttk

try:
    import yt_dlp
    from yt_dlp.utils import DownloadCancelled, DownloadError
except ImportError:  # 第一次執行、還沒裝 yt-dlp
    yt_dlp = None
    DownloadCancelled = DownloadError = Exception

APP_NAME = "AnyVideo Download"
APP_VERSION = "1.0.0"

# ─── 格式選項 ──────────────────────────────────────────────────────────────

VIDEO_CONTAINERS = ["mp4", "mkv", "webm", "mov", "avi"]
VIDEO_QUALITIES = [
    ("最佳", None), ("2160p", 2160), ("1440p", 1440), ("1080p", 1080),
    ("720p", 720), ("480p", 480), ("360p", 360),
]
# 影片的取捨。左邊是顯示文字，右邊是存進設定檔的值 —— 設定檔存值而不存文字，
# 以後改顯示文字時舊的設定檔才不會失效。
PRIORITIES = [
    ("畫質優先 — 最高解析度，可能是 AV1／VP9", "quality"),
    ("相容優先 — H.264，到哪都能播", "compat"),
    # 不轉檔：網站提供的最高畫質影像與最高音質聲音，原封不動封裝（串流複製）。能放進 mp4 才用 mp4，否則 mkv。
    # 與來源逐格相同，沒有任何損耗；代價是 AV1／VP9／Opus 在舊電視、舊手機、部分剪輯軟體上不一定能播。
    ("原始品質 — 不轉檔，無損耗（常是 mkv）", "original"),
]
# 相容優先時保證輸出 H.264 的容器。webm 容器規格裝不下 H.264，avi 另有自己的
# 固定編碼（MPEG-4 Part 2），所以不在這裡。
COMPAT_H264_CONTAINERS = {"mp4", "mkv", "mov"}

AUDIO_FORMATS = ["mp3", "m4a", "opus", "ogg", "flac", "wav"]
# 音訊的「原始格式」：不轉檔，存成網站原本的聲音（opus、m4a…）。設定檔存 "best"（yt-dlp --audio-format 的寫法）。
AUDIO_ORIGINAL_LABEL = "原始（不轉檔）"
AUDIO_ORIGINAL_VALUE = "best"
AUDIO_BITRATES = ["320", "256", "192", "128", "96"]
LOSSLESS_AUDIO = {"flac", "wav"}          # 無損格式沒有位元率可選

# 能嵌入封面／字幕的容器。塞進不支援的容器，ffmpeg 會讓整筆後處理失敗，
# 所以不支援的時候直接把勾選框停用，而不是讓使用者勾了才失敗。
THUMB_OK = {"mp4", "mkv", "mov", "mp3", "m4a", "opus", "ogg", "flac"}
SUBS_OK = {"mp4", "mkv", "webm", "mov"}

BROWSERS = [("不使用", None), ("Firefox", "firefox"), ("Edge", "edge"), ("Chrome", "chrome")]

# 後處理器的內部名稱 -> 狀態列顯示的文字
PP_LABELS = {
    "Merger": "合併影像與聲音",
    "VideoConvertor": "轉換影片格式",
    "VideoRemuxer": "重新封裝",
    "ExtractAudio": "轉出音訊",
    "Metadata": "寫入標題等資訊",
    "EmbedThumbnail": "嵌入封面",
    "EmbedSubtitle": "嵌入字幕",
    "MoveFiles": "整理檔案",
    "EnsureH264": "轉成相容編碼 H.264（比較慢）",
}

# 背景呼叫 ffmpeg / ffprobe 時不要彈出主控台視窗
_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0


# ─── 設定 ──────────────────────────────────────────────────────────────────

def _settings_path() -> Path:
    base = os.environ.get("APPDATA") or str(Path.home())
    return Path(base) / "anyvideo_download" / "settings.json"


@dataclass
class Settings:
    out_dir: str = str(Path.home() / "Downloads")
    mode: str = "video"                    # "video" 或 "audio"
    container: str = "mp4"
    quality: str = "最佳"
    priority: str = "quality"              # "quality" 或 "compat"，見 PRIORITIES
    audio_format: str = "mp3"
    audio_bitrate: str = "192"
    playlist: bool = False
    embed_meta: bool = True
    embed_thumb: bool = False
    embed_subs: bool = False
    browser: str = "不使用"
    cookies_file: str = ""                 # cookies.txt 的路徑；檔案存在時優先於 browser

    @classmethod
    def load(cls) -> "Settings":
        try:
            data = json.loads(_settings_path().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            return cls()
        known = {f.name for f in fields(cls)}
        return cls(**{k: v for k, v in data.items() if k in known})

    def save(self) -> None:
        p = _settings_path()
        try:
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(json.dumps(asdict(self), ensure_ascii=False, indent=2), encoding="utf-8")
        except OSError:
            pass   # 存不了設定不該讓下載失敗


# ─── 下載核心（與介面無關，可單獨測試）────────────────────────────────────

def find_ffmpeg() -> str | None:
    exe = shutil.which("ffmpeg")
    return str(Path(exe).parent) if exe else None


def usable_cookies_file(s: Settings) -> str:
    """設定的 cookies.txt 路徑；沒設或檔案不存在時回傳空字串（改用瀏覽器的設定）。"""
    p = (s.cookies_file or "").strip()
    return p if p and Path(p).is_file() else ""


COOKIE_MAX_BYTES = 5 * 1024 * 1024


def _read_cookie_candidate(path: str) -> tuple[bool, bool]:
    """(是不是 cookies.txt, 內容有沒有抖音／TikTok 的 cookie)。不合格或讀不到回傳 (False, False)。"""
    try:
        if os.path.getsize(path) > COOKIE_MAX_BYTES:
            return False, False
        with open(path, "rb") as f:
            data = f.read(COOKIE_MAX_BYTES + 1)
    except OSError:
        return False, False
    if not data or len(data) > COOKIE_MAX_BYTES:
        return False, False
    text = data.decode("utf-8", "ignore").lstrip("\ufeff")
    valid = "Netscape HTTP Cookie File" in text[:4096] or "HTTP Cookie File" in text[:4096]
    if not valid:
        for line in text.split("\n"):
            line = line.rstrip("\r")
            if not line.strip():
                continue
            if line.startswith("#HttpOnly_"):
                line = line[len("#HttpOnly_"):]
            if not line.startswith("#") and len(line.split("\t")) == 7:
                valid = True
                break
    if not valid:
        return False, False
    return True, ("douyin.com" in text or "tiktok.com" in text)


def cookie_file_has_douyin(path: str) -> bool:
    return _read_cookie_candidate(path)[1]


def detect_cookie_files(dirs: list[str]) -> list[str]:
    """在指定資料夾（不含子資料夾）找匯出的 cookies.txt，回傳有效的檔案，最適合的排最前面。
    排序：含 douyin.com／tiktok.com → 檔名含 cookie → 修改時間新的 → 路徑字母順序。"""
    seen: set[str] = set()
    found: list[tuple[bool, bool, float, str]] = []
    for d in dirs:
        if not d:
            continue
        try:
            names = os.listdir(d)
        except OSError:
            continue
        for name in names:
            if not name.lower().endswith(".txt"):
                continue
            path = os.path.abspath(os.path.join(d, name))
            key = os.path.normcase(path)
            if key in seen or not os.path.isfile(path):
                continue
            seen.add(key)
            valid, douyin = _read_cookie_candidate(path)
            if not valid:
                continue
            try:
                mtime = os.path.getmtime(path)
            except OSError:
                continue
            found.append((douyin, "cookie" in name.lower(), mtime, path))
    found.sort(key=lambda t: (not t[0], not t[1], -t[2], t[3]))
    return [t[3] for t in found]


def default_cookie_dirs(out_dir: str = "") -> list[str]:
    """自動偵測 cookies.txt 要找的資料夾。"""
    home = os.environ.get("USERPROFILE") or str(Path.home())
    dirs = [os.path.join(home, "Downloads"), os.path.join(home, "Desktop"), os.path.join(home, "Documents")]
    for var in ("OneDrive", "OneDriveConsumer", "OneDriveCommercial"):
        od = os.environ.get(var)
        if od:
            dirs += [os.path.join(od, "Downloads"), os.path.join(od, "Desktop"), os.path.join(od, "Documents")]
    appdata = os.environ.get("APPDATA")
    if appdata:
        dirs.append(os.path.join(appdata, "anyvideo_download"))
    if (out_dir or "").strip():
        dirs.append(out_dir.strip())
    dirs.append(str(Path(__file__).resolve().parent))
    return dirs


def apply_cookie_options(opts: dict, s: Settings) -> None:
    """cookies.txt 優先（同時給瀏覽器會去讀被鎖住的資料庫而出錯），否則用瀏覽器 cookies。"""
    cf = usable_cookies_file(s)
    if cf:
        opts["cookiefile"] = cf
        return
    browser = dict(BROWSERS).get(s.browser)
    if browser:
        opts["cookiesfrombrowser"] = (browser,)


def build_ydl_options(s: Settings, ffmpeg_dir: str | None) -> dict:
    """依設定組出 yt-dlp 的參數。回呼（logger、progress hook）由呼叫端補上。"""
    out = Path(s.out_dir)
    if s.playlist:
        tmpl = "%(playlist_title,playlist|playlist).80B/%(playlist_index|0)03d - %(title).150B [%(id)s].%(ext)s"
    else:
        # .150B 是以位元組截斷標題，避免超過 Windows 的路徑長度限制
        tmpl = "%(title).150B [%(id)s].%(ext)s"

    opts: dict = {
        "outtmpl": str(out / tmpl),
        "windowsfilenames": True,
        "noplaylist": not s.playlist,
        # 播放清單中某一部下載失敗時繼續下一部，而不是整份清單中止
        "ignoreerrors": "only_download",
        "quiet": True,
        "noprogress": True,
        "color": {"stdout": "never", "stderr": "never"},
        # 網路不穩是下載失敗的大宗：次數放寬、每次重試前等久一點（1,2,4…秒，最多 20 秒），
        # 並設逾時，避免連線卡死而永遠不重試。
        "retries": 10,
        "fragment_retries": 10,
        "extractor_retries": 5,
        "socket_timeout": 30,
        "retry_sleep_functions": {kind: (lambda n: min(2 ** n, 20)) for kind in ("http", "fragment")},
        "concurrent_fragment_downloads": 4,
    }
    if ffmpeg_dir:
        opts["ffmpeg_location"] = ffmpeg_dir

    apply_cookie_options(opts, s)

    pps: list[dict] = []

    if s.mode == "video":
        h = dict(VIDEO_QUALITIES).get(s.quality)
        hf = f"[height<={h}]" if h else ""
        opts["format"] = f"bestvideo*{hf}+bestaudio/best{hf}/best"
        c = s.container

        if s.priority == "original":
            # 原始品質：不指定排序（yt-dlp 自己的順序就是畫質優先：解析度、幀率、HDR、編碼、位元率，聲音挑位元率最高）、
            # 不轉檔、不重新封裝成別的容器、不碰 EnsureH264。合併只是把影像與聲音串流複製進同一個容器：
            # 兩邊都放得進 mp4 就用 mp4，否則 mkv（什麼都裝得下）。結果與網站提供的串流解碼後逐格相同。
            opts["merge_output_format"] = "mp4/mkv"
            if s.embed_subs:
                opts["writesubtitles"] = True
                opts["subtitleslangs"] = ["zh.*", "en.*"]
                pps.append({"key": "FFmpegEmbedSubtitle", "already_have_subtitle": False})
            if s.embed_meta:
                pps.append({"key": "FFmpegMetadata", "add_metadata": True, "add_chapters": True})
            opts["postprocessors"] = pps
            return opts

        # 合併容器與轉檔策略。原則是「能直接封裝就不要重新編碼」。
        #
        # merge_output_format 支援「/」備援：依序找第一個與所選編碼相容的容器，
        # mkv 什麼都裝得下，所以放最後當保底。FFmpegVideoConvertor 遇到已經是
        # 目標格式的檔案會自動略過，只有真的落到 mkv（或網站只給單一其他格式）
        # 時才重新編碼。
        #
        # ── 畫質優先 vs 相容優先 ──
        #
        # 差別只在 format_sort 裡「解析度」與「編碼」誰排前面。
        #
        # 畫質優先：先比解析度。網站最高畫質若只有 AV1 版本就挑 AV1 —— 畫面最好，
        #   但舊手機、電視、剪輯軟體常常播不了。
        # 相容優先：先比編碼。vcodec:h264 會把 H.264 排第一，比它「新」的 H.265、
        #   VP9、AV1 反而排到最後（yt-dlp 對有序欄位加上 :值 的語意就是如此）。
        #   解析度退居第二，所以同樣是 H.264 時仍然挑最高畫質。
        #
        # format_sort 只是「偏好」。網站若根本沒有 H.264，挑到的仍是別的編碼，
        # 所以相容優先在最後還有一道 EnsureH264PP 把關，見下方。
        compat = s.priority == "compat"
        if compat:
            opts["format_sort"] = (["vcodec:vp9", "acodec:opus", "res", "ext:webm:webm"] if c == "webm"
                                   else ["vcodec:h264", "acodec:aac", "res", "ext:mp4:m4a"])
        else:
            opts["format_sort"] = {
                "webm": ["res", "ext:webm:webm"],
                "mkv": ["res"],
                # mov 裝不下 AV1／VP9／Opus。解析度仍然第一，但同解析度時挑 H.264，
                # 省掉下面 EnsureH264 的重新編碼。
                "mov": ["res", "vcodec:h264", "acodec:aac", "ext:mp4:m4a"],
            }.get(c, ["res", "ext:mp4:m4a"])

        opts["merge_output_format"] = {"mkv": "mkv", "webm": "webm/mkv"}.get(c, "mp4/mkv")

        # 後處理順序：EnsureH264 → 重新封裝／轉檔 → 字幕 → 資訊 → 封面。
        # 這也是 yt-dlp 命令列固定的順序（--use-postprocessor 永遠排最前面），
        # Qt 版與 C# 版走命令列，這裡照同一個順序，三個版本的結果才會一樣。
        #
        # EnsureH264 必須在寫入資訊／封面／字幕之前：重新編碼只保留第一條影像與
        # 聲音串流，排在後面會把剛嵌進去的東西洗掉。
        if c == "mov" or (compat and c in COMPAT_H264_CONTAINERS):
            # 先把不相容的那一邊重新編碼成 H.264／AAC，之後一定能直接改封裝，
            # 不必再轉檔一次。
            # mov 在兩種取捨都要這一道：mov 裝不下 AV1／VP9／Opus
            # （ffmpeg：「av1 only supported in MP4」），畫質優先挑到 AV1 時不處理會直接失敗。
            pps.append({"key": "EnsureH264",
                        "video_ok": ["h264"] if compat else ["h264", "hevc"]})
            pps.append({"key": "FFmpegVideoRemuxer", "preferedformat": c})
        elif c == "mkv":
            # mkv 接受任何編碼，所以單一格式來源用「重新封裝」就好，不必重編碼
            pps.append({"key": "FFmpegVideoRemuxer", "preferedformat": "mkv"})
        else:  # mp4（畫質優先）、webm、avi
            pps.append({"key": "FFmpegVideoConvertor", "preferedformat": c})

        if s.embed_subs and c in SUBS_OK:
            opts["writesubtitles"] = True
            opts["subtitleslangs"] = ["zh.*", "en.*"]
            pps.append({"key": "FFmpegEmbedSubtitle", "already_have_subtitle": False})
        target = c
    else:
        opts["format"] = "bestaudio/best"
        codec = {"ogg": "vorbis"}.get(s.audio_format, s.audio_format)
        pp = {"key": "FFmpegExtractAudio", "preferredcodec": codec}
        # 「原始（不轉檔）」= preferredcodec "best"：聲音串流原封不動存成它原本的格式，沒有重新編碼
        if s.audio_format not in LOSSLESS_AUDIO and s.audio_format != AUDIO_ORIGINAL_VALUE:
            pp["preferredquality"] = s.audio_bitrate
        pps.append(pp)
        target = s.audio_format

    if s.embed_meta:
        pps.append({"key": "FFmpegMetadata", "add_metadata": True, "add_chapters": True})
    if s.embed_thumb and target in THUMB_OK:
        opts["writethumbnail"] = True
        pps.append({"key": "EmbedThumbnail", "already_have_thumbnail": False})

    opts["postprocessors"] = pps
    return opts


def _fmt_bytes(n: float | None) -> str:
    if not n:
        return "?"
    for unit in ("B", "KB", "MB", "GB"):
        if n < 1024:
            return f"{n:.1f} {unit}"
        n /= 1024
    return f"{n:.1f} TB"


def _fmt_eta(sec: float | None) -> str:
    if sec is None:
        return "?"
    sec = int(sec)
    return f"{sec // 3600:d}:{sec // 60 % 60:02d}:{sec % 60:02d}" if sec >= 3600 else f"{sec // 60:d}:{sec % 60:02d}"


def _probe_codecs(path: str, ffmpeg_dir: str | None) -> tuple[str | None, str | None]:
    """回傳（影像編碼, 聲音編碼）。嵌入的封面圖也是一條影像串流，要略過。"""
    ffprobe = str(Path(ffmpeg_dir) / "ffprobe") if ffmpeg_dir else "ffprobe"
    r = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries",
         "stream=codec_type,codec_name:stream_disposition=attached_pic", "-of", "json", path],
        capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)
    try:
        streams = json.loads(r.stdout or "{}").get("streams", [])
    except ValueError:
        return None, None
    vcodec = acodec = None
    for st in streams:
        if st.get("codec_type") == "video" and not (st.get("disposition") or {}).get("attached_pic"):
            vcodec = vcodec or st.get("codec_name")
        elif st.get("codec_type") == "audio":
            acodec = acodec or st.get("codec_name")
    return vcodec, acodec


if yt_dlp is not None:
    from yt_dlp.postprocessor import get_postprocessor
    from yt_dlp.postprocessor.common import PostProcessor
    from yt_dlp.utils import PostProcessingError

    class EnsureH264PP(PostProcessor):
        """編碼把關：影像不在 video_ok 裡、或聲音不是 AAC／MP3，就重新編碼。

        format_sort 只能表達「偏好」。網站若根本不提供 H.264（有些平台的高畫質
        只有 AV1），挑到的仍然是 AV1。選了相容優先的人要的是「到哪都能播」，
        所以最後在這裡把關。

        已經相容的檔案什麼都不做 —— 只有真的不相容時才花時間重新編碼，而且
        只重編碼不相容的那一邊（例如影像已是 H.264、只有聲音是 Opus，影像就直接複製）。
        """

        # 這些容器裝得下 H.264；其他的（例如網站只給一個 .webm）重新編碼時改存 mkv
        H264_CONTAINERS = {".mp4", ".m4v", ".mkv", ".mov"}

        def __init__(self, downloader=None, ffmpeg_dir=None, video_ok=("h264",)):
            super().__init__(downloader)
            self._ffmpeg_dir = ffmpeg_dir
            self._video_ok = set(video_ok)

        def run(self, info):
            path = info.get("filepath")
            if not path or not os.path.exists(path):
                return [], info

            vcodec, acodec = _probe_codecs(path, self._ffmpeg_dir)
            v_ok = vcodec is None or vcodec in self._video_ok
            a_ok = acodec in (None, "aac", "mp3")
            if v_ok and a_ok:
                self.to_screen(f"已是相容編碼（{vcodec}/{acodec}），不需要重新編碼")
                return [], info

            self.to_screen(f"網站沒有提供相容編碼，重新編碼 {vcodec}/{acodec} → h264/aac，會比較慢")
            ffmpeg = str(Path(self._ffmpeg_dir) / "ffmpeg") if self._ffmpeg_dir else "ffmpeg"
            src = Path(path)
            suffix = src.suffix if src.suffix.lower() in self.H264_CONTAINERS else ".mkv"
            dst = src.with_suffix(suffix)
            # 暫存檔保留副檔名結尾，ffmpeg 靠副檔名決定輸出容器
            tmp = src.with_name(src.stem + ".compat-tmp" + suffix)
            cmd = [ffmpeg, "-y", "-loglevel", "error", "-i", str(src),
                   "-map", "0:v:0", "-map", "0:a:0?", "-map_metadata", "0"]
            cmd += (["-c:v", "copy"] if v_ok else
                    ["-c:v", "libx264", "-preset", "fast", "-crf", "20", "-pix_fmt", "yuv420p"])
            cmd += ["-c:a", "copy"] if a_ok else ["-c:a", "aac", "-b:a", "192k"]
            if suffix.lower() in (".mp4", ".m4v", ".mov"):
                cmd += ["-movflags", "+faststart"]     # 讓播放器不必讀完整個檔才能開始播
            cmd.append(str(tmp))

            r = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                               errors="replace", creationflags=_NO_WINDOW)
            if r.returncode != 0 or not tmp.exists():
                tmp.unlink(missing_ok=True)
                raise PostProcessingError("重新編碼成 H.264 失敗：" + (r.stderr or "").strip()[-300:])
            os.replace(tmp, dst)
            if dst == src:
                return [], info
            # 換了容器：告訴後面的後處理器新檔名，舊檔交給 yt-dlp 刪除
            info["filepath"], info["ext"] = str(dst), dst.suffix[1:]
            return [str(src)], info


def have_impersonation() -> bool:
    """curl_cffi 裝了沒有。Vimeo、受 Cloudflare 保護的網站需要它偽裝成瀏覽器才連得進去。"""
    try:
        import curl_cffi  # noqa: F401
        return True
    except ImportError:
        return False


def core_age_days() -> int | None:
    """yt-dlp 版本（日期格式 2026.08.19）距今幾天。網站常改版，太舊就會壞。"""
    if yt_dlp is None:
        return None
    try:
        y, m, d = (int(x) for x in yt_dlp.version.__version__.split(".")[:3])
        return (datetime.now() - datetime(y, m, d)).days
    except ValueError:
        return None


# 錯誤訊息分類。順序重要：先比對的優先。每項是（類別, 關鍵字的正規表示式）。
_ERROR_KINDS = [
    ("drm", r"DRM"),
    ("disk", r"No space left|Permission denied|File name too long|path too long|Errno 28|Errno 13"),
    ("bot", r"confirm you.re not a bot|Sign in to confirm"),
    ("login", r"[Ll]og(?:ged)?[- ]?in|[Ss]ign in|private video|members-only|[Cc]ookies|requires authentication|HTTP Error 401"),
    ("geo", r"geo[- ]?restrict|not available in your country|in your location"),
    ("impersonate", r"impersonat|HTTP Error 403|HTTP Error 429|Cloudflare|Just a moment|\b403\b"),
    # 憑證驗證失敗不是網路不穩：重試一百次結果都一樣，所以放在 network 之前，單獨一類、不重試
    ("cert", r"CERTIFICATE_VERIFY_FAILED|certificate verify failed|self[- ]signed certificate|unable to get local issuer"),
    ("format", r"[Rr]equested format is not available|No video formats found"),
    ("network", r"timed? ?out|Temporary failure|Connection (?:reset|aborted|refused)|IncompleteRead|"
                r"RemoteDisconnected|Name or service not known|getaddrinfo|SSL|HTTP Error 5\d\d|"
                r"Unable to download (?:webpage|JSON)|EOF occurred"),
    ("ffmpeg", r"ffmpeg|ffprobe|Postprocessing"),
    ("parse", r"Cannot parse data"),
    ("update", r"Unsupported URL|Unable to extract|nsig|signature|player response|"
               r"Failed to parse JSON|Unable to download.*(?:API|player)|HTTP Error 404"),
]


def classify_error(text: str) -> str:
    import re
    for kind, pattern in _ERROR_KINDS:
        if re.search(pattern, text):
            return kind
    return "unknown"


def error_hint(kind: str, s: Settings) -> str | None:
    """失敗原因 → 使用者看得懂、做得到的下一步。"""
    age = core_age_days()
    old = f"（目前的下載核心已 {age} 天沒更新）" if age and age > 30 else ""
    hints = {
        "drm": "這個內容有 DRM 保護，無法下載。",
        "disk": "寫入失敗：請確認儲存位置還有空間、有寫入權限，且路徑不要太深。",
        "bot": "網站要求證明不是機器人。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。",
        "login": "這個內容需要登入。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。"
                 if s.browser == "不使用" else
                 "已使用瀏覽器 cookies 仍要求登入：請確認有在該瀏覽器登入，並先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies）。",
        "geo": "網站限制地區，需要該地區的網路（VPN／代理）才能下載。",
        "impersonate": "網站擋掉了程式的連線。" + (
            "" if have_impersonation() else
            "缺少瀏覽器偽裝元件 curl_cffi：按「更新下載核心」安裝後重新開啟程式。"),
        "cert": "HTTPS 憑證驗證失敗：這台電腦連到這個網站時，拿到的憑證不是網站本身的（網路供應商、公司防火牆、防毒軟體的 HTTPS 掃描，"
                "或網站被導向到別的伺服器）。重試沒有用，請檢查網路環境；不要為了下載而關閉憑證檢查。",
        "network": "網路連線不穩或被中斷，已自動重試仍失敗。請檢查網路／VPN 後再試，續傳會接著下載。",
        "ffmpeg": "ffmpeg 處理失敗：確認 ffmpeg 可用（winget install Gyan.FFmpeg），或換一種輸出格式。",
        "parse": "網站回傳的頁面解析不了。最常見是私人／朋友限定／社團內容（Facebook、Instagram 等）："
                 "「登入狀態」選一個已登入該網站的瀏覽器再試；公開內容仍失敗才是網站改版，請更新下載核心。",
        "update": f"網站可能改版了{old}：按「更新下載核心」，重新開啟程式再試。",
    }
    return hints.get(kind)


def plan_retry(kind: str, tried: set[str]) -> tuple[dict, str] | None:
    """依失敗類別決定「換個方式再試一次」要改哪些選項。沒有值得一試的辦法就回傳 None。

    每一種辦法只試一次（tried 記錄已試過的），所以最多重試幾次是有上限的。
    """
    if kind in tried:
        return None
    if kind == "network":
        return {}, "網路不穩，稍後重試…"
    if kind == "format":
        # 指定的畫質／編碼組合不存在：放掉排序與高度限制，交給 yt-dlp 挑能用的
        return {"format": "bv*+ba/b", "format_sort": None}, "找不到符合的格式，改用寬鬆的格式選擇重試…"
    if kind == "impersonate" and have_impersonation():
        from yt_dlp.networking.impersonate import ImpersonateTarget
        return {"impersonate": ImpersonateTarget.from_str("chrome")}, "改以 Chrome 瀏覽器身分連線重試…"
    return None


# ─── 下載前的設定防呆 ───────────────────────────────────────────────────────
#
# 按「開始下載」前先檢查：哪些設定其實做不到、會悄悄被略過、或根本寫不進去。每一項都是「確定有問題」才提醒（不是你刻意的選擇都要念一次），
# 能修的附上修法，修法以「畫質最高又不損耗」為優先（例如改成 mkv：什麼都裝得下，只是重新封裝，不必重新編碼）。
#   致命（不能照原設定做）：dir_admin_only 儲存位置在需要管理員權限的地方、dir_not_writable 儲存位置寫不進去
#   提醒：disk_low 磁碟空間不到 2 GB、compat_not_kept 相容優先卻選了 webm／avi（做不到 H.264）、
#         subs_dropped／thumb_dropped 勾了嵌入字幕／封面但容器不支援（會悄悄不嵌）
# 規則與 qt/src/guard.cpp、csharp/Guard.cs 相同（tests/guard_flow.py 三個版本跑同一批案例）。

import tempfile
from dataclasses import replace

GUARD_MIN_FREE_BYTES = 2 * 1024 ** 3


def _norm_path(raw: str) -> str:
    out = raw.replace("\\", "/").lower()
    while len(out) > 1 and out.endswith("/"):
        out = out[:-1]
    if len(out) >= 4 and out[:4] in ("//?/", "//./"):                  # \\?\C:\... 與 \\.\C:\... 是同一個磁碟路徑的另一種寫法
        out = out[4:]
    if len(out) >= 6 and out.startswith("/mnt/") and "a" <= out[5] <= "z" and (len(out) == 6 or out[6] == "/"):   # WSL 的 /mnt/c/windows 就是 c:/windows
        out = out[5] + ":" + out[6:]
    return out


def _starts_with_folder(path: str, folder: str) -> bool:
    return path == folder or (len(path) > len(folder) and path.startswith(folder) and path[len(folder)] == "/")


def is_admin_only_path(raw: str) -> bool:
    """需要管理員權限才能寫的地方（Windows 系統資料夾、Program Files、磁碟根目錄、Unix 的系統資料夾）。程式永遠不寫那裡。"""
    p = _norm_path(raw.strip())
    if not p:
        return True                                                       # 沒有路徑不是可以寫的地方
    if len(p) >= 2 and p[1] == ":" and "a" <= p[0] <= "z":
        rest = p[3:] if len(p) > 3 else ""
        if not rest:
            return True                                                   # 磁碟根目錄本身
        if "/" not in rest and "." in rest:                               # 直接放在根目錄的檔案；根目錄底下自己建的資料夾（D:/影片）另外判斷
            return True
        return any(_starts_with_folder(rest, f) for f in ("windows", "program files", "program files (x86)", "programdata",
                                                           "$recycle.bin", "system volume information", "recovery"))
    return any(p == f or (f != "/" and _starts_with_folder(p, f))
               for f in ("/", "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/opt", "/var", "/boot", "/sys", "/proc", "/dev", "/root", "/srv", "/snap"))


@dataclass
class GuardIssue:
    id: str
    detail: str = ""
    fatal: bool = False
    fix: tuple = ()          # （設定名稱, 值）的清單；空＝沒有自動修正


def probe_out_dir(out_dir: str) -> dict:
    """儲存位置的實際狀況：寫得進去嗎、還剩多少空間（None＝不知道）。資料夾要已經存在。"""
    try:
        with tempfile.NamedTemporaryFile(dir=out_dir, prefix=".avd_write_test"):
            writable = True
    except OSError:
        writable = False
    try:
        free = shutil.disk_usage(out_dir).free
    except OSError:
        free = None
    return {"writable": writable, "free": free}


def check_download_settings(s: "Settings", writable: bool | None = True, free: int | None = None) -> list[GuardIssue]:
    """設定與儲存位置的狀況 → 有問題的清單。writable／free 由 probe_out_dir 取得（測試時直接給）。"""
    issues: list[GuardIssue] = []
    if is_admin_only_path(s.out_dir):
        issues.append(GuardIssue("dir_admin_only", s.out_dir, fatal=True))
    elif writable is False:
        issues.append(GuardIssue("dir_not_writable", s.out_dir, fatal=True))
    elif free is not None and free < GUARD_MIN_FREE_BYTES:
        issues.append(GuardIssue("disk_low", f"{free / 1024 ** 3:.1f}"))

    if (s.cookies_file or "").strip() and not usable_cookies_file(s):
        issues.append(GuardIssue("cookies_missing", s.cookies_file.strip(), fix=(("cookies_file", ""),)))

    video, original, c = s.mode == "video", s.priority == "original", s.container
    if video and s.priority == "compat" and c in ("webm", "avi"):
        issues.append(GuardIssue("compat_not_kept", c, fix=(("container", "mp4"),)))
    if video and not original and s.embed_subs and c not in SUBS_OK:
        issues.append(GuardIssue("subs_dropped", c, fix=(("container", "mkv"),)))
    if s.embed_thumb:
        if video and original:
            issues.append(GuardIssue("thumb_dropped", "original", fix=(("embed_thumb", "false"),)))
        elif video and c not in THUMB_OK:
            issues.append(GuardIssue("thumb_dropped", c, fix=(("container", "mkv"),)))
        elif not video and s.audio_format not in THUMB_OK:
            issues.append(GuardIssue("thumb_dropped", s.audio_format,
                                     fix=(("audio_format", "flac"),) if s.audio_format == "wav" else (("embed_thumb", "false"),)))
    return issues


def apply_guard_fixes(s: "Settings", issues: list[GuardIssue]) -> "Settings":
    """把每個問題的修法套上去（依序，後面的蓋過前面的）。回傳新的設定，原本的不動。"""
    changes: dict = {}
    for i in issues:
        for key, value in i.fix:
            changes[key] = (value == "true") if key.startswith("embed_") else value
    return replace(s, **changes)


def describe_guard_issue(i: GuardIssue) -> str:
    d = i.detail
    if i.id == "dir_admin_only":
        return f"儲存位置 {d} 在需要管理員權限的位置（Windows、Program Files 等），程式不會寫入那裡。請選擇你自己的資料夾（例如「文件」或「影片」底下）。"
    if i.id == "dir_not_writable":
        return f"無法寫入儲存位置 {d}：資料夾是唯讀、被 Windows「受控資料夾存取」（防勒索軟體）擋住，或權限不足。請換一個位置（例如桌面）。"
    if i.id == "disk_low":
        return f"儲存位置所在的磁碟只剩 {d} GB 可用空間。高畫質影片可能好幾 GB，空間不夠時下載到一半會失敗。"
    if i.id == "cookies_missing":
        return f"指定的 cookies.txt 找不到：{d}。這次不會用到它（改用「登入狀態」選的瀏覽器）。可以清除這個設定，或重新選擇檔案。"
    if i.id == "compat_not_kept":
        return f"「相容優先」承諾輸出 H.264 + AAC，但 {d} 容器做不到（webm 裝不下 H.264，avi 一律轉成 MPEG-4 + MP3），結果不會是 H.264。要 H.264 請改用 mp4（畫質最高又能播）。"
    if i.id == "subs_dropped":
        return f"已勾選嵌入字幕，但 {d} 容器不支援，字幕不會被嵌入。改用 mkv 可以嵌入（也不必重新編碼）。"
    if i.id == "thumb_dropped":
        if d == "original":
            return "「原始品質（不轉檔）」要等網站給什麼才知道容器，所以不會嵌入封面；勾選的「嵌入封面」不會有作用。"
        if d == "best":
            return "音訊「原始（不轉檔）」不嵌入封面（格式要看網站給什麼才知道）；勾選的「嵌入封面」不會有作用。"
        if d == "wav":
            return "wav 不支援嵌入封面。改用 flac（同樣無損）可以嵌入。"
        return f"已勾選嵌入封面，但 {d} 容器不支援，封面不會被嵌入。改用 mkv 可以嵌入。"
    return i.id


# ─── 平台網址處理 ───────────────────────────────────────────────────────────
#
# 使用者貼的常常不是乾淨的網址。抖音與小紅書的「分享」會複製一整段文字（標題、表情符號、「復制打開抖音」之類的話，
# 中間夾著 https://v.douyin.com/xxxx/ 這樣的短連結）；瀏覽器網址列的抖音網址常是 douyin.com/jingxuan?modal_id=123，
# 小紅書常是 xiaohongshu.com/user/profile/<用戶>/<筆記>。yt-dlp 的解析器只認 douyin.com/video/<id> 與
# xiaohongshu.com/explore/<id>，其他形式要先改寫，否則一律「Unsupported URL」。
# 規則與 qt/src/platform.cpp、csharp/Platform.cs 相同（tests/platform_urls.py 三個版本跑同一批案例）。

import re
import urllib.error
import urllib.parse
import urllib.request

PLATFORM_NAMES = {"youtube": "YouTube", "douyin": "抖音", "tiktok": "TikTok", "xiaohongshu": "小紅書"}

# 這些字元不會是網址的一部分，貼上的分享文字裡它們把網址和下一句話隔開
_STOP = "\\s<>\"'`\u3000，。！？、；：（）【】「」『』《》〈〉…“”‘’"
_FULL_URL = re.compile("https?://[^" + _STOP + "]+", re.I)
# 分享文字常省略 https://：只認得這幾個已知網域，避免把一般文字當成網址
_BARE_URL = re.compile(r"(?<![/\w.])((?:v\.douyin\.com|xhslink\.com|vm\.tiktok\.com|vt\.tiktok\.com|www\.xiaohongshu\.com|"
                       r"www\.douyin\.com|www\.tiktok\.com|youtu\.be)/[^" + _STOP + "]+)", re.I)


def _host_is(host: str, domain: str) -> bool:
    return host == domain or host.endswith("." + domain)


def platform_of(url: str) -> str:
    """"youtube"、"douyin"、"tiktok"、"xiaohongshu"，其他回傳空字串。"""
    host = (urllib.parse.urlsplit(url).hostname or "").lower()
    if any(_host_is(host, d) for d in ("youtube.com", "youtu.be", "youtube-nocookie.com")):
        return "youtube"
    if any(_host_is(host, d) for d in ("douyin.com", "iesdouyin.com", "douyinvod.com")):
        return "douyin"
    if _host_is(host, "tiktok.com"):
        return "tiktok"
    if any(_host_is(host, d) for d in ("xiaohongshu.com", "xhslink.com", "xhscdn.com")):
        return "xiaohongshu"
    return ""


def platform_name(platform: str) -> str:
    return PLATFORM_NAMES.get(platform, "")


def extract_urls(text: str) -> list[str]:
    """一段文字裡所有的網址（依出現順序、不重複）。去掉網址後面黏著的標點（含全形的 。，！？）、「」等。"""
    hits = [(m.start(), m.group()) for m in _FULL_URL.finditer(text)]
    for m in _BARE_URL.finditer(text):
        if not any(pos <= m.start(1) < pos + len(u) for pos, u in hits):          # 已經被有 https:// 的涵蓋就不重複算
            hits.append((m.start(1), "https://" + m.group(1)))
    out: list[str] = []
    for _, u in sorted(hits):
        u = u.rstrip(".,;:!?)]}>*")                                               # 句尾的標點不是網址
        if urllib.parse.urlsplit(u).hostname and u not in out:
            out.append(u)
    return out


def douyin_video_id(url: str) -> str:
    """抖音網址裡的影片 id（douyin.com/video/ID、?modal_id=ID、iesdouyin.com/share/video/ID）；沒有回傳空字串。"""
    sp = urllib.parse.urlsplit(url)
    host = (sp.hostname or "").lower()
    if not (_host_is(host, "douyin.com") or _host_is(host, "iesdouyin.com")):
        return ""
    if m := re.search(r"/(?:video|share/video|note|share/note)/(\d+)", sp.path):
        return m.group(1)
    modal = urllib.parse.parse_qs(sp.query).get("modal_id", [""])[0]            # 精選、推薦、用戶頁裡點開的影片
    return modal if modal.isdigit() else ""


def normalize_url(raw: str) -> str:
    """改寫成 yt-dlp 認得的形式；不需要改寫的原樣回傳。小紅書網址裡的 xsec_token 等參數必須保留（沒有它網站不給內容）。"""
    url = raw.strip()
    sp = urllib.parse.urlsplit(url)
    host = (sp.hostname or "").lower()
    platform = platform_of(url)
    if platform == "douyin":
        if "/note/" in sp.path:                       # 圖文：yt-dlp 沒有解析器，不改寫
            return url
        vid = douyin_video_id(url)
        return f"https://www.douyin.com/video/{vid}" if vid else url
    if platform == "xiaohongshu" and _host_is(host, "xiaohongshu.com"):
        # /user/profile/<用戶>/<筆記>、/search_result/<筆記>、/explore/<筆記> → /explore/<筆記>；查詢參數原樣保留
        if m := re.search(r"/(?:user/profile/[0-9a-f]+|search_result|explore|discovery/item)/([0-9a-f]{16,})", sp.path):
            return f"https://www.xiaohongshu.com/explore/{m.group(1)}" + ("?" + sp.query if sp.query else "")
    return url


def is_short_link(url: str, extra_hosts=()) -> bool:
    """短連結：要先跟著轉址走，才知道真正的影片網址（v.douyin.com、xhslink.com）。"""
    host = (urllib.parse.urlsplit(url).hostname or "").lower()
    return host in extra_hosts or host == "v.douyin.com" or _host_is(host, "xhslink.com")


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):        # 自己一步一步走，每一步都看看是不是已經是真正的網址
        return None


_MOBILE_UA = ("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 "
              "(KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1")


def resolve_link(url: str, extra_hosts=(), timeout: float = 15) -> tuple[str, str]:
    """把貼上的網址變成可以交給 yt-dlp 的網址：先改寫，是短連結就跟著轉址走到真正的網址再改寫一次。
    轉址用手機瀏覽器的身分（分享連結是給手機開的，電腦版身分常被導去登入頁）。

    回傳（最終網址, 說明）。說明是給紀錄區看的一行（沒有話說時是空字串）；展開失敗時最終網址是原來的，說明講原因。"""
    original, current = url, normalize_url(url)
    opener = urllib.request.build_opener(_NoRedirect)
    for hops in range(7):
        if not is_short_link(current, extra_hosts):
            plat = platform_of(current)
            note = f"辨識為{platform_name(plat)}：{current}" if current != original and plat else ""
            return current, note
        if hops >= 6:
            return original, f"短連結轉址超過 6 次，放棄展開：{original}"
        req = urllib.request.Request(current, headers={"User-Agent": _MOBILE_UA})
        try:
            with opener.open(req, timeout=timeout) as resp:
                location = resp.headers.get("Location")
                why = "沒有轉址"
        except urllib.error.HTTPError as e:
            location, why = e.headers.get("Location"), f"HTTP {e.code}"
            if not location:
                why = f"HTTP {e.code}"
        except (urllib.error.URLError, OSError) as e:
            return original, f"短連結展開失敗（{e}）：{original}"
        if not location:
            return original, f"短連結展開失敗（{why}）：{original}"
        current = normalize_url(urllib.parse.urljoin(current, location.strip()))
    return original, f"短連結轉址超過 6 次，放棄展開：{original}"


def capture_is_good_fallback(platform: str) -> bool:
    """這個平台的下載失敗後，改用「從瀏覽器擷取」常常有效（yt-dlp 的解析器常因網站改版而壞）。"""
    return platform in ("douyin", "xiaohongshu", "tiktok")


def platform_hint(platform: str, kind: str, error_text: str, uses_cookies: bool, cookies_from_file: bool = False) -> str | None:
    """這個平台下載失敗時，使用者該知道的事。kind 是 classify_error 的分類；沒有特別的話回傳 None。"""
    if platform == "douyin":
        if cookies_from_file and ("fresh cookies" in error_text.lower() or kind == "login"):
            return ("抖音要求「新的 cookies」：你指定的 cookies.txt 可能已過期，或不是從 douyin.com 匯出的。"
                    "請先用瀏覽器開 douyin.com、隨便看一支影片讓網站發給瀏覽器 cookies，再重新匯出 cookies.txt。"
                    "還是不行就用「從瀏覽器擷取」：瀏覽器能播，就能抓。")
        if "fresh cookies" in error_text.lower() or kind == "login":
            return ("抖音要求「新的 cookies」（不一定要登入）：先用 Firefox 或 Chrome 開 douyin.com、隨便看一支影片讓網站發給瀏覽器 cookies，"
                    "再到「登入狀態」選那個瀏覽器。"
                    + ("已經選了瀏覽器仍失敗：請先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies），或改用 Firefox。" if uses_cookies else "")
                    + "還是不行就用「從瀏覽器擷取」：瀏覽器能播，就能抓。")
        if kind == "update" or "Unsupported URL" in error_text:
            return "這個抖音網址形式 yt-dlp 不支援（例如圖文 /note/）或網站改版了。可以改用「從瀏覽器擷取」。"
    if platform == "xiaohongshu":
        if "certificate" in error_text.lower() or "SSL" in error_text:
            return ("連到小紅書時憑證驗證失敗：你目前的網路把 xiaohongshu.com 導向了別的伺服器（憑證不是小紅書的），"
                    "所以不是影片或程式的問題。這是網路層的限制，程式沒辦法處理；在能正常連到小紅書的網路環境下就可以下載。")
        if kind in ("login", "update") or "Unsupported URL" in error_text:
            return ("小紅書的網址要帶 xsec_token 參數才給內容：請貼「分享」複製出來的完整網址（含 ?xsec_token=…），不要自己刪掉後面的參數。"
                    "短連結 xhslink.com 程式會自己展開。仍不行可以改用「從瀏覽器擷取」。")
    if platform == "tiktok" and kind in ("impersonate", "update"):
        return "TikTok 常擋非瀏覽器的連線：按「更新下載核心」安裝瀏覽器偽裝元件（curl_cffi）；仍不行可以改用「從瀏覽器擷取」。"
    if platform == "youtube" and kind in ("bot", "login"):
        return ("YouTube 要求證明不是機器人：「登入狀態」選一個已登入 YouTube 的瀏覽器（Firefox 最可靠）。"
                "另外請確認已安裝 deno（winget install DenoLand.Deno），YouTube 需要它才能取得完整格式。")
    return None


class _YdlLogger:
    """把 yt-dlp 的訊息轉進介面的紀錄區。順便記下錯誤，失敗時用來判斷原因。"""

    def __init__(self, post):
        self.post = post
        self.errors: list[str] = []

    def debug(self, msg: str) -> None:
        # yt-dlp 的一般訊息也走 debug()。真正的除錯訊息帶 [debug] 前綴，略過；
        # 下載百分比已經有進度條，不再逐行洗版。
        if msg.startswith("[debug] ") or (msg.startswith("[download]") and "%" in msg):
            return
        self.post("log", msg, "info")

    def info(self, msg: str) -> None:
        self.post("log", msg, "info")

    def warning(self, msg: str) -> None:
        self.post("log", msg, "warn")

    def error(self, msg: str) -> None:
        self.errors.append(msg)
        self.post("log", msg, "error")


class DownloadJob(threading.Thread):
    """依序下載多個網址。所有對外溝通都透過 post(kind, *args)。"""

    def __init__(self, urls, settings: Settings, ffmpeg_dir, post, cancel: threading.Event):
        super().__init__(daemon=True)
        self.urls = urls
        self.settings = settings
        self.ffmpeg_dir = ffmpeg_dir
        self.post = post
        self.cancel = cancel
        self.capture_urls: list[str] = []      # 失敗了、而且改用「從瀏覽器擷取」常常有效的網址（抖音、小紅書、TikTok）
        self._last_post = 0.0

    def _progress(self, d: dict) -> None:
        # yt-dlp 在下載迴圈裡呼叫這裡，所以取消在這裡拋例外最即時
        if self.cancel.is_set():
            raise DownloadCancelled("使用者取消")
        status = d.get("status")
        info = d.get("info_dict") or {}
        if status == "downloading":
            now = time.monotonic()
            if now - self._last_post < 0.15:      # 節流，避免塞爆佇列
                return
            self._last_post = now
            done = d.get("downloaded_bytes") or 0
            total = d.get("total_bytes") or d.get("total_bytes_estimate")
            pct = done * 100 / total if total else None
            if pct is None and d.get("fragment_count"):
                pct = (d.get("fragment_index") or 0) * 100 / d["fragment_count"]
            part = "音訊" if info.get("vcodec") == "none" else ("影像" if info.get("acodec") == "none" else "")
            text = f"{part + ' ' if part else ''}{_fmt_bytes(done)} / {_fmt_bytes(total)}" \
                   f"   {_fmt_bytes(d.get('speed'))}/s   剩餘 {_fmt_eta(d.get('eta'))}"
            if info.get("playlist_index") and info.get("n_entries"):
                text = f"清單 {info['playlist_index']}/{info['n_entries']}   " + text
            self.post("progress", pct, text)
        elif status == "finished":
            self.post("progress", 100.0, "下載完成，後處理中…")

    def _add_postprocessors(self, ydl, pp_defs: list[dict]) -> None:
        for d in pp_defs:
            d = dict(d)
            key = d.pop("key")
            when = d.pop("when", "post_process")
            if key == "EnsureH264":
                pp = EnsureH264PP(ydl, ffmpeg_dir=self.ffmpeg_dir, **d)
            else:
                pp = get_postprocessor(key)(ydl, **d)
            ydl.add_post_processor(pp, when=when)

    def _postprocess(self, d: dict) -> None:
        if d.get("status") == "started":
            if self.cancel.is_set():
                raise DownloadCancelled("使用者取消")
            name = d.get("postprocessor") or ""
            self.post("status", PP_LABELS.get(name, name) + "…")

    def run(self) -> None:
        ok = fail = 0
        cancelled = False
        base_opts = build_ydl_options(self.settings, self.ffmpeg_dir)
        logger = _YdlLogger(self.post)
        base_opts["logger"] = logger
        base_opts["progress_hooks"] = [self._progress]
        base_opts["postprocessor_hooks"] = [self._postprocess]
        # 後處理器自己一個一個加，不交給 opts["postprocessors"]：yt-dlp 只認得
        # 內建的 key，EnsureH264PP 不在其中，而它又必須插在轉檔之後、寫入資訊之前。
        pp_defs = base_opts.pop("postprocessors", [])

        for i, url in enumerate(self.urls, 1):
            if self.cancel.is_set():
                cancelled = True
                break
            self.post("item", i, len(self.urls), url)
            # 改寫成 yt-dlp 認得的網址（抖音的 modal_id、小紅書的用戶頁網址…），短連結先展開
            shown_url = url
            url, note = resolve_link(url)
            if note:
                self.post("log", note, "info")
            # 失敗時依原因換個方式再試（見 plan_retry），每種辦法只試一次
            opts, tried = dict(base_opts), set()
            success = False
            while True:
                logger.errors.clear()
                reason = ""
                try:
                    with yt_dlp.YoutubeDL(opts) as ydl:
                        self._add_postprocessors(ydl, pp_defs)
                        success = ydl.download([url]) == 0
                except DownloadCancelled:
                    cancelled = True
                    break
                except DownloadError as e:
                    reason = str(e)       # 原因 logger 已經印過了
                except Exception as e:     # 不讓單一網址的意外中斷整批
                    reason = f"{type(e).__name__}: {e}"
                    self.post("log", f"（{reason}）", "error")
                if success or cancelled:
                    break
                kind = classify_error("\n".join(logger.errors) or reason)
                plan = plan_retry(kind, tried)
                if plan is None or self.cancel.is_set():
                    uses_cookies = bool(usable_cookies_file(self.settings)) or dict(BROWSERS).get(self.settings.browser) is not None
                    hint = (platform_hint(platform_of(url), kind, "\n".join(logger.errors) or reason, uses_cookies,
                                          bool(usable_cookies_file(self.settings)))
                            or error_hint(kind, self.settings))                # 這個平台特有的說明優先
                    if hint:
                        self.post("log", "建議：" + hint, "warn")
                    if capture_is_good_fallback(platform_of(url)) and url not in self.capture_urls:
                        self.capture_urls.append(url)
                    break
                tried.add(kind)
                overrides, message = plan
                if kind == "network":
                    self.cancel.wait(3)
                self.post("log", message, "warn")
                opts = {**opts, **overrides}
                opts = {k: v for k, v in opts.items() if v is not None}
            if cancelled:
                break
            if success:
                ok += 1
                self.post("log", f"完成：{url}", "ok")
            else:
                fail += 1
                self.post("log", f"失敗：{url}", "error")
        self.post("done", ok, fail, cancelled)


class ProbeJob(threading.Thread):
    """只解析、不下載：看標題、長度、可用的畫質。"""

    def __init__(self, url, settings: Settings, post):
        super().__init__(daemon=True)
        self.url, self.settings, self.post = url, settings, post

    def run(self) -> None:
        opts = {
            "quiet": True, "no_warnings": False, "skip_download": True,
            "extract_flat": "in_playlist", "noplaylist": not self.settings.playlist,
            "logger": _YdlLogger(self.post),
        }
        apply_cookie_options(opts, self.settings)
        url, note = resolve_link(self.url)
        if note:
            self.post("log", note, "info")
        try:
            with yt_dlp.YoutubeDL(opts) as ydl:
                info = ydl.extract_info(url, download=False)
        except Exception as e:
            hint = error_hint(classify_error(str(e)), self.settings)
            self.post("probe", None, f"解析失敗：{e}" + (f"\n建議：{hint}" if hint else ""))
            return
        if not info:
            self.post("probe", None, "解析失敗：沒有取得任何資訊")
            return

        lines = []
        if info.get("_type") == "playlist":
            n = len(info.get("entries") or [])
            lines.append(f"播放清單：{info.get('title') or '(無標題)'}")
            lines.append(f"共 {n} 項" + ("" if self.settings.playlist else "　—　要全部下載請勾選「下載整個播放清單」"))
        else:
            lines.append(f"標題：{info.get('title') or '(無標題)'}")
            if info.get("uploader"):
                lines.append(f"上傳者：{info['uploader']}")
            if info.get("duration"):
                lines.append(f"長度：{_fmt_eta(info['duration'])}")
            heights = sorted({f["height"] for f in info.get("formats") or [] if f.get("height")}, reverse=True)
            if heights:
                lines.append("可用畫質：" + "、".join(f"{h}p" for h in heights))
            lines.append(f"網站：{info.get('extractor_key') or info.get('extractor') or '?'}")
        self.post("probe", info, "\n".join(lines))


class PipJob(threading.Thread):
    """在背景更新（或安裝）yt-dlp。網站改版時下載核心常需要跟著更新。"""

    def __init__(self, post):
        super().__init__(daemon=True)
        self.post = post

    def run(self) -> None:
        cmd = [sys.executable.replace("pythonw.exe", "python.exe"), "-m", "pip", "install", "-U", "yt-dlp[default,curl-cffi]"]
        flags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
        try:
            p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                 text=True, encoding="utf-8", errors="replace", creationflags=flags)
            for line in p.stdout:
                line = line.rstrip()
                if line:
                    self.post("log", line, "info")
            rc = p.wait()
        except OSError as e:
            self.post("log", f"無法執行 pip：{e}", "error")
            rc = -1
        self.post("pip_done", rc)


# ─── 介面 ──────────────────────────────────────────────────────────────────

class App(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title(f"{APP_NAME} {APP_VERSION}")
        self.minsize(820, 640)
        self.geometry("900x740")

        self.settings = Settings.load()
        self.q: queue.Queue = queue.Queue()
        self.ffmpeg_dir = find_ffmpeg()
        self.cancel = threading.Event()
        self.job: DownloadJob | None = None
        self.busy = False

        self._init_style()
        self._build()
        self._load_settings_into_ui()
        self._update_option_states()
        self.after(100, self._poll)
        self.protocol("WM_DELETE_WINDOW", self._on_close)
        self.after(200, self._startup_checks)

    # ── 外觀 ─────────────────────────────────────────────────────────────
    def _init_style(self) -> None:
        font = ("Microsoft JhengHei UI", 10) if sys.platform == "win32" else ("TkDefaultFont", 10)
        self.option_add("*Font", font)
        style = ttk.Style(self)
        style.configure(".", font=font)
        style.configure("Accent.TButton", font=(font[0], 10, "bold"))
        style.configure("Hint.TLabel", foreground="#666666")
        self._font = font

    # ── 版面 ─────────────────────────────────────────────────────────────
    def _build(self) -> None:
        root = ttk.Frame(self, padding=10)
        root.pack(fill="both", expand=True)
        root.columnconfigure(0, weight=1)
        root.rowconfigure(4, weight=1)

        # 網址
        f_url = ttk.LabelFrame(root, text="網址（每行一個，可一次貼多個）", padding=8)
        f_url.grid(row=0, column=0, sticky="ew")
        f_url.columnconfigure(0, weight=1)
        self.url_text = tk.Text(f_url, height=5, wrap="none", undo=True)
        self.url_text.grid(row=0, column=0, sticky="ew")
        sb = ttk.Scrollbar(f_url, orient="vertical", command=self.url_text.yview)
        sb.grid(row=0, column=1, sticky="ns")
        self.url_text.configure(yscrollcommand=sb.set)
        self._attach_context_menu(self.url_text)

        row = ttk.Frame(f_url)
        row.grid(row=1, column=0, columnspan=2, sticky="ew", pady=(6, 0))
        ttk.Button(row, text="貼上", command=self._paste_urls).pack(side="left")
        ttk.Button(row, text="清除", command=lambda: self.url_text.delete("1.0", "end")).pack(side="left", padx=4)
        self.btn_probe = ttk.Button(row, text="解析第一個網址", command=self._probe)
        self.btn_probe.pack(side="left", padx=4)

        # 輸出設定
        f_opt = ttk.LabelFrame(root, text="輸出設定", padding=8)
        f_opt.grid(row=1, column=0, sticky="ew", pady=(8, 0))
        for c in (1, 3, 5):
            f_opt.columnconfigure(c, weight=1)

        self.var_mode = tk.StringVar()
        m = ttk.Frame(f_opt)
        m.grid(row=0, column=0, columnspan=6, sticky="w")
        ttk.Label(m, text="下載類型：").pack(side="left")
        ttk.Radiobutton(m, text="影片", value="video", variable=self.var_mode,
                        command=self._update_option_states).pack(side="left", padx=(0, 12))
        ttk.Radiobutton(m, text="僅音訊", value="audio", variable=self.var_mode,
                        command=self._update_option_states).pack(side="left")

        self.var_container = tk.StringVar()
        self.var_quality = tk.StringVar()
        ttk.Label(f_opt, text="影片格式").grid(row=1, column=0, sticky="w", pady=(8, 0))
        self.cb_container = ttk.Combobox(f_opt, textvariable=self.var_container, values=VIDEO_CONTAINERS,
                                         state="readonly", width=10)
        self.cb_container.grid(row=1, column=1, sticky="w", pady=(8, 0))
        self.cb_container.bind("<<ComboboxSelected>>", lambda e: self._update_option_states())
        ttk.Label(f_opt, text="畫質").grid(row=1, column=2, sticky="w", pady=(8, 0))
        self.cb_quality = ttk.Combobox(f_opt, textvariable=self.var_quality,
                                       values=[q[0] for q in VIDEO_QUALITIES], state="readonly", width=10)
        self.cb_quality.grid(row=1, column=3, sticky="w", pady=(8, 0))

        self.var_afmt = tk.StringVar()
        self.var_abr = tk.StringVar()
        # 取捨（只對影片有效）
        self.var_priority = tk.StringVar()
        ttk.Label(f_opt, text="取捨").grid(row=2, column=0, sticky="w", pady=(6, 0))
        self.cb_priority = ttk.Combobox(f_opt, textvariable=self.var_priority,
                                        values=[p[0] for p in PRIORITIES], state="readonly", width=34)
        self.cb_priority.grid(row=2, column=1, columnspan=5, sticky="w", pady=(6, 0))
        self.cb_priority.bind("<<ComboboxSelected>>", lambda e: self._update_option_states())

        ttk.Label(f_opt, text="音訊格式").grid(row=3, column=0, sticky="w", pady=(6, 0))
        self.cb_afmt = ttk.Combobox(f_opt, textvariable=self.var_afmt, values=AUDIO_FORMATS + [AUDIO_ORIGINAL_LABEL],
                                    state="readonly", width=10)
        self.cb_afmt.grid(row=3, column=1, sticky="w", pady=(6, 0))
        self.cb_afmt.bind("<<ComboboxSelected>>", lambda e: self._update_option_states())
        ttk.Label(f_opt, text="位元率 (kbps)").grid(row=3, column=2, sticky="w", pady=(6, 0))
        self.cb_abr = ttk.Combobox(f_opt, textvariable=self.var_abr, values=AUDIO_BITRATES,
                                   state="readonly", width=10)
        self.cb_abr.grid(row=3, column=3, sticky="w", pady=(6, 0))

        self.var_playlist = tk.BooleanVar()
        self.var_meta = tk.BooleanVar()
        self.var_thumb = tk.BooleanVar()
        self.var_subs = tk.BooleanVar()
        chk = ttk.Frame(f_opt)
        chk.grid(row=4, column=0, columnspan=6, sticky="w", pady=(8, 0))
        ttk.Checkbutton(chk, text="下載整個播放清單", variable=self.var_playlist).pack(side="left", padx=(0, 12))
        ttk.Checkbutton(chk, text="寫入標題等資訊", variable=self.var_meta).pack(side="left", padx=(0, 12))
        self.chk_thumb = ttk.Checkbutton(chk, text="嵌入封面縮圖", variable=self.var_thumb)
        self.chk_thumb.pack(side="left", padx=(0, 12))
        self.chk_subs = ttk.Checkbutton(chk, text="嵌入字幕（中文／英文）", variable=self.var_subs)
        self.chk_subs.pack(side="left")

        self.var_browser = tk.StringVar()
        ttk.Label(f_opt, text="登入狀態").grid(row=5, column=0, sticky="w", pady=(8, 0))
        cb_b = ttk.Combobox(f_opt, textvariable=self.var_browser, values=[b[0] for b in BROWSERS],
                            state="readonly", width=10)
        cb_b.grid(row=5, column=1, sticky="w", pady=(8, 0))
        ttk.Label(f_opt, text="需要登入才能看的內容，借用瀏覽器已登入的 cookies",
                  style="Hint.TLabel").grid(row=5, column=2, columnspan=4, sticky="w", pady=(8, 0))

        self.var_cookies = tk.StringVar()
        ttk.Label(f_opt, text="cookies.txt").grid(row=6, column=0, sticky="w", pady=(8, 0))
        ck = ttk.Frame(f_opt)
        ck.grid(row=6, column=1, columnspan=5, sticky="ew", pady=(8, 0))
        ck.columnconfigure(0, weight=1)
        ttk.Entry(ck, textvariable=self.var_cookies, state="readonly").grid(row=0, column=0, sticky="ew")
        self.cookie_btns = [
            ttk.Button(ck, text="選擇…", command=self._choose_cookies),
            ttk.Button(ck, text="自動偵測", command=self._detect_cookies),
            ttk.Button(ck, text="清除", command=lambda: self.var_cookies.set("")),
        ]
        self.cookie_btns[0].grid(row=0, column=1, padx=4)
        self.cookie_btns[1].grid(row=0, column=2)
        self.cookie_btns[2].grid(row=0, column=3, padx=(4, 0))

        self.var_outdir = tk.StringVar()
        ttk.Label(f_opt, text="儲存位置").grid(row=7, column=0, sticky="w", pady=(8, 0))
        out = ttk.Frame(f_opt)
        out.grid(row=7, column=1, columnspan=5, sticky="ew", pady=(8, 0))
        out.columnconfigure(0, weight=1)
        ttk.Entry(out, textvariable=self.var_outdir).grid(row=0, column=0, sticky="ew")
        ttk.Button(out, text="瀏覽…", command=self._choose_dir).grid(row=0, column=1, padx=4)
        ttk.Button(out, text="開啟資料夾", command=self._open_dir).grid(row=0, column=2)

        # 動作
        act = ttk.Frame(root)
        act.grid(row=2, column=0, sticky="ew", pady=(10, 0))
        self.btn_start = ttk.Button(act, text="開始下載", style="Accent.TButton", command=self._start)
        self.btn_start.pack(side="left")
        self.btn_cancel = ttk.Button(act, text="取消", command=self._cancel, state="disabled")
        self.btn_cancel.pack(side="left", padx=6)
        self.btn_capture = ttk.Button(act, text="從瀏覽器擷取…", command=self._capture)
        self.btn_capture.pack(side="left")
        self.btn_update = ttk.Button(act, text="更新下載核心", command=self._update_core)
        self.btn_update.pack(side="right")

        # 進度
        prog = ttk.Frame(root)
        prog.grid(row=3, column=0, sticky="ew", pady=(10, 0))
        prog.columnconfigure(0, weight=1)
        self.lbl_item = ttk.Label(prog, text="待機")
        self.lbl_item.grid(row=0, column=0, sticky="w")
        self.bar = ttk.Progressbar(prog, maximum=100)
        self.bar.grid(row=1, column=0, sticky="ew", pady=(4, 2))
        self.lbl_status = ttk.Label(prog, text="", style="Hint.TLabel")
        self.lbl_status.grid(row=2, column=0, sticky="w")

        # 紀錄
        f_log = ttk.LabelFrame(root, text="紀錄", padding=6)
        f_log.grid(row=4, column=0, sticky="nsew", pady=(8, 0))
        f_log.columnconfigure(0, weight=1)
        f_log.rowconfigure(0, weight=1)
        self.log = tk.Text(f_log, height=10, wrap="word", state="disabled")
        self.log.grid(row=0, column=0, sticky="nsew")
        sb2 = ttk.Scrollbar(f_log, orient="vertical", command=self.log.yview)
        sb2.grid(row=0, column=1, sticky="ns")
        self.log.configure(yscrollcommand=sb2.set)
        self.log.tag_configure("info", foreground="#222222")
        self.log.tag_configure("ok", foreground="#1a7f37")
        self.log.tag_configure("warn", foreground="#b35900")
        self.log.tag_configure("error", foreground="#c62828")
        self.log.tag_configure("probe", foreground="#1f4e8c")
        ttk.Button(f_log, text="清除紀錄", command=self._clear_log).grid(row=1, column=0, sticky="e", pady=(4, 0))

        # 狀態列
        self.lbl_env = ttk.Label(root, text="", style="Hint.TLabel")
        self.lbl_env.grid(row=5, column=0, sticky="w", pady=(6, 0))
        self._refresh_env_label()

        self._inputs = [self.url_text, self.cb_container, self.cb_quality, self.cb_priority,
                        self.cb_afmt, self.cb_abr,
                        cb_b, self.chk_thumb, self.chk_subs]

    def _attach_context_menu(self, widget: tk.Text) -> None:
        menu = tk.Menu(widget, tearoff=0)
        menu.add_command(label="貼上", command=lambda: widget.event_generate("<<Paste>>"))
        menu.add_command(label="剪下", command=lambda: widget.event_generate("<<Cut>>"))
        menu.add_command(label="複製", command=lambda: widget.event_generate("<<Copy>>"))
        menu.add_separator()
        menu.add_command(label="全選", command=lambda: widget.tag_add("sel", "1.0", "end"))
        menu.add_command(label="清除", command=lambda: widget.delete("1.0", "end"))
        widget.bind("<Button-3>", lambda e: menu.tk_popup(e.x_root, e.y_root))

    # ── 設定 <-> 介面 ─────────────────────────────────────────────────────
    def _load_settings_into_ui(self) -> None:
        s = self.settings
        self.var_mode.set(s.mode)
        self.var_container.set(s.container if s.container in VIDEO_CONTAINERS else "mp4")
        self.var_quality.set(s.quality if s.quality in dict(VIDEO_QUALITIES) else "最佳")
        labels = {value: label for label, value in PRIORITIES}
        self.var_priority.set(labels.get(s.priority, PRIORITIES[0][0]))
        self.var_afmt.set(AUDIO_ORIGINAL_LABEL if s.audio_format == AUDIO_ORIGINAL_VALUE
                          else s.audio_format if s.audio_format in AUDIO_FORMATS else "mp3")
        self.var_abr.set(s.audio_bitrate if s.audio_bitrate in AUDIO_BITRATES else "192")
        self.var_playlist.set(s.playlist)
        self.var_meta.set(s.embed_meta)
        self.var_thumb.set(s.embed_thumb)
        self.var_subs.set(s.embed_subs)
        self.var_browser.set(s.browser if s.browser in dict(BROWSERS) else "不使用")
        self.var_cookies.set(s.cookies_file)
        self.var_outdir.set(s.out_dir)

    def _collect_settings(self) -> Settings:
        s = Settings(
            out_dir=self.var_outdir.get().strip(),
            mode=self.var_mode.get(),
            container=self.var_container.get(),
            quality=self.var_quality.get(),
            priority=dict(PRIORITIES).get(self.var_priority.get(), "quality"),
            audio_format=AUDIO_ORIGINAL_VALUE if self.var_afmt.get() == AUDIO_ORIGINAL_LABEL else self.var_afmt.get(),
            audio_bitrate=self.var_abr.get(),
            playlist=self.var_playlist.get(),
            embed_meta=self.var_meta.get(),
            embed_thumb=self.var_thumb.get(),
            embed_subs=self.var_subs.get(),
            browser=self.var_browser.get(),
            cookies_file=self.var_cookies.get().strip(),
        )
        self.settings = s
        return s

    def _update_option_states(self) -> None:
        video = self.var_mode.get() == "video"
        busy = "disabled" if self.busy else "readonly"
        original = dict(PRIORITIES).get(self.var_priority.get()) == "original"     # 不轉檔：容器由來源決定
        audio_original = self.var_afmt.get() == AUDIO_ORIGINAL_LABEL
        self.cb_container.configure(state=busy if video and not original else "disabled")
        self.cb_quality.configure(state=busy if video else "disabled")
        self.cb_priority.configure(state=busy if video else "disabled")
        self.cb_afmt.configure(state="disabled" if video else busy)
        lossless = self.var_afmt.get() in LOSSLESS_AUDIO
        self.cb_abr.configure(state="disabled" if (video or lossless or audio_original) else busy)

        target = ("" if original else self.var_container.get()) if video else self.var_afmt.get()
        thumb_ok = target in THUMB_OK and not self.busy                   # 容器還不知道時（原始品質、原始音訊）不嵌封面
        subs_ok = video and (original or self.var_container.get() in SUBS_OK) and not self.busy
        self.chk_thumb.configure(state="normal" if thumb_ok else "disabled")
        self.chk_subs.configure(state="normal" if subs_ok else "disabled")

    def _refresh_env_label(self) -> None:
        ytv = yt_dlp.version.__version__ if yt_dlp else "未安裝"
        ff = "已找到" if self.ffmpeg_dir else "未找到"
        self.lbl_env.configure(text=f"yt-dlp {ytv}　｜　ffmpeg {ff}　｜　Python {sys.version.split()[0]}")

    # ── 動作 ─────────────────────────────────────────────────────────────
    def _startup_checks(self) -> None:
        self._log(f"{APP_NAME} {APP_VERSION} 已啟動", "info")
        if not self.ffmpeg_dir:
            self._log("找不到 ffmpeg：影音合併與格式轉換都需要它。安裝方式："
                      "winget install Gyan.FFmpeg（裝完重新開啟本程式）", "warn")
        if not shutil.which("deno"):
            self._log("提示：未安裝 deno。YouTube 需要 JavaScript 執行環境才能取得完整格式，"
                      "若 YouTube 下載失敗或畫質選項很少，請執行 winget install DenoLand.Deno", "warn")
        if yt_dlp is not None and not have_impersonation():
            self._log("提示：未安裝瀏覽器偽裝元件 curl_cffi，Vimeo 等會擋程式的網站會下載失敗。"
                      "按「更新下載核心」即可安裝。", "warn")
        age = core_age_days()
        if age is not None and age > 30:
            self._log(f"提示：下載核心已 {age} 天沒更新。網站常改版，下載失敗時請先按「更新下載核心」。", "warn")
        if yt_dlp is None:
            self.btn_start.configure(state="disabled")
            self.btn_probe.configure(state="disabled")
            if messagebox.askyesno(APP_NAME, "尚未安裝下載核心 yt-dlp。\n\n要現在自動安裝嗎？"):
                self._update_core()

    def _urls(self) -> list[str]:
        # 貼上的常常是整段分享文字（抖音、小紅書的「分享」會連標題與表情符號一起複製），從裡面找出網址；
        # 一行裡沒有網址的（例如 yt-dlp 認得的影片代號）原樣保留。
        out: list[str] = []
        for ln in self.url_text.get("1.0", "end").splitlines():
            t = ln.strip()
            if not t or t.startswith("#"):
                continue
            found = extract_urls(t)
            for u in (found or [t]):
                n = normalize_url(u) if found else u
                if n not in out:
                    out.append(n)
        return out

    def _paste_urls(self) -> None:
        try:
            text = self.clipboard_get()
        except tk.TclError:
            return
        existing = self.url_text.get("1.0", "end").strip()
        if existing and not existing.endswith("\n"):
            self.url_text.insert("end", "\n")
        self.url_text.insert("end", text.strip() + "\n")

    def _choose_dir(self) -> None:
        d = filedialog.askdirectory(initialdir=self.var_outdir.get() or str(Path.home()))
        if d:
            self.var_outdir.set(str(Path(d)))

    def _choose_cookies(self) -> None:
        cur = self.var_cookies.get()
        f = filedialog.askopenfilename(title="選擇 cookies.txt", filetypes=[("cookies.txt", "*.txt"), ("所有檔案", "*.*")],
                                       initialdir=str(Path(cur).parent) if cur and Path(cur).parent.is_dir() else str(Path.home()))
        if f:
            self.var_cookies.set(str(Path(f)))

    def _detect_cookies(self) -> None:
        if self.busy:
            return
        found = detect_cookie_files(default_cookie_dirs(self.var_outdir.get()))
        if not found:
            messagebox.showinfo(APP_NAME, "找不到 cookies.txt。\n\n請先用瀏覽器擴充功能（例如 Get cookies.txt LOCALLY）在 douyin.com 匯出，"
                                          "檔案放在「下載」資料夾，再按一次自動偵測。")
            return
        best = found[0]
        self.var_cookies.set(best)
        when = datetime.fromtimestamp(os.path.getmtime(best)).strftime("%Y-%m-%d %H:%M")
        messagebox.showinfo(APP_NAME, f"已選用：{best}\n修改時間：{when}\n含抖音/TikTok cookies："
                                      f"{'是' if cookie_file_has_douyin(best) else '否'}\n\n共找到 {len(found)} 個可用的檔案。")

    def _open_dir(self) -> None:
        d = Path(self.var_outdir.get())
        if not d.exists():
            messagebox.showinfo(APP_NAME, "資料夾還不存在，第一次下載時會自動建立。")
            return
        if sys.platform == "win32":
            os.startfile(d)
        else:
            subprocess.Popen(["xdg-open", str(d)])

    def _probe(self) -> None:
        urls = self._urls()
        if not urls:
            messagebox.showinfo(APP_NAME, "請先貼上網址。")
            return
        self._log(f"解析中：{urls[0]}", "info")
        self.btn_probe.configure(state="disabled")
        ProbeJob(urls[0], self._collect_settings(), self._post).start()

    def _start(self) -> None:
        urls = self._urls()
        if not urls:
            messagebox.showinfo(APP_NAME, "請先貼上至少一個網址。")
            return
        s = self._collect_settings()
        if not s.out_dir:
            messagebox.showwarning(APP_NAME, "請選擇儲存位置。")
            return
        if not self.ffmpeg_dir:
            messagebox.showwarning(APP_NAME, "找不到 ffmpeg，無法合併影音或轉換格式。\n\n"
                                             "請先安裝：winget install Gyan.FFmpeg\n裝完後重新開啟本程式。")
            return
        try:
            Path(s.out_dir).mkdir(parents=True, exist_ok=True)
        except OSError as e:
            messagebox.showerror(APP_NAME, f"無法建立儲存資料夾：\n{e}")
            return
        s = self._guard_before_download(s)
        if s is None:
            return
        s.save()

        self.cancel.clear()
        self._set_busy(True)
        prio = "相容優先" if s.priority == "compat" else "原始品質（不轉檔）" if s.priority == "original" else "畫質優先"
        if s.mode == "video":
            what = f"影片 {s.quality} / {prio}" if s.priority == "original" else f"影片 {s.container} / {s.quality} / {prio}"
        elif s.audio_format == AUDIO_ORIGINAL_VALUE:
            what = "音訊 原始格式（不轉檔）"
        else:
            what = f"音訊 {s.audio_format}" + ("" if s.audio_format in LOSSLESS_AUDIO else f" / {s.audio_bitrate} kbps")
        self._log(f"開始下載 {len(urls)} 個網址（{what}）→ {s.out_dir}", "info")
        self.job = DownloadJob(urls, s, self.ffmpeg_dir, self._post, self.cancel)
        self.job.start()

    def _guard_before_download(self, s: Settings) -> Settings | None:
        """下載前的設定防呆（見 check_download_settings）。回傳要用的設定；使用者取消或遇到致命問題回傳 None。"""
        issues = check_download_settings(s, **probe_out_dir(s.out_dir))
        fatal = [i for i in issues if i.fatal]
        if fatal:
            messagebox.showerror(APP_NAME, "\n\n".join(describe_guard_issue(i) for i in fatal))
            return None
        if not issues:
            return s
        text = "\n\n".join("• " + describe_guard_issue(i) for i in issues)
        if any(i.fix for i in issues):
            answer = messagebox.askyesnocancel(APP_NAME, text + "\n\n要自動修正嗎？（修正後保留最高畫質）\n\n"
                                               "是：自動修正並開始　　否：照原設定開始　　取消：不開始")
            if answer is None:
                return None
            if answer:
                s = apply_guard_fixes(s, issues)
                self.settings = s
                self._load_settings_into_ui()
                self._update_option_states()
            return s
        return s if messagebox.askokcancel(APP_NAME, text + "\n\n要照原設定開始嗎？") else None

    def _capture(self, start_url: str = "") -> None:
        """「從瀏覽器擷取」：網站頁面解析不了、但瀏覽器能播時，抓出播放器實際載入的影音，自動挑最高畫質與音質。"""
        s = self._collect_settings()
        if not s.out_dir:
            messagebox.showwarning(APP_NAME, "請選擇儲存位置。")
            return
        try:
            from avd_capture import open_dialog
        except ImportError as e:
            messagebox.showerror(APP_NAME, f"缺少 avd_capture.py 或其相依套件：{e}\n\n（websockets 隨 yt-dlp[default] 安裝；按「更新下載核心」可補裝。）")
            return
        if not start_url:
            urls = self._urls()
            start_url = urls[0] if urls else ""
        open_dialog(self, s.out_dir, self.ffmpeg_dir, start_url)

    def _cancel(self) -> None:
        self.cancel.set()
        self.btn_cancel.configure(state="disabled")
        self.lbl_status.configure(text="取消中…（轉檔進行中時會在這一步結束後停止）")

    def _update_core(self) -> None:
        self._log("更新下載核心 yt-dlp …", "info")
        self.btn_update.configure(state="disabled")
        PipJob(self._post).start()

    def _set_busy(self, busy: bool) -> None:
        self.busy = busy
        self.btn_start.configure(state="disabled" if busy else "normal")
        self.btn_probe.configure(state="disabled" if busy else "normal")
        self.btn_update.configure(state="disabled" if busy else "normal")
        self.btn_capture.configure(state="disabled" if busy else "normal")
        self.btn_cancel.configure(state="normal" if busy else "disabled")
        for b in self.cookie_btns:
            b.configure(state="disabled" if busy else "normal")
        self.url_text.configure(state="disabled" if busy else "normal")
        self._update_option_states()
        if not busy:
            self._bar_set(0)

    def _on_close(self) -> None:
        if self.busy and not messagebox.askyesno(APP_NAME, "還在下載中，確定要結束嗎？"):
            return
        self.cancel.set()
        try:
            self._collect_settings().save()
        except Exception:
            pass
        self.destroy()

    # ── 執行緒 -> 介面 ───────────────────────────────────────────────────
    def _post(self, kind: str, *args) -> None:
        """下載執行緒呼叫這裡。只放進佇列，絕不直接碰元件。"""
        self.q.put((kind, args))

    def _poll(self) -> None:
        try:
            while True:
                kind, args = self.q.get_nowait()
                self._handle(kind, args)
        except queue.Empty:
            pass
        self.after(100, self._poll)

    def _handle(self, kind: str, args: tuple) -> None:
        if kind == "log":
            self._log(args[0], args[1])
        elif kind == "item":
            i, n, url = args
            self.lbl_item.configure(text=f"第 {i}/{n} 個：{url}")
            self._bar_set(0)
            self.lbl_status.configure(text="解析網址中…")
        elif kind == "progress":
            pct, text = args
            self._bar_set(pct)
            self.lbl_status.configure(text=text)
        elif kind == "status":
            self._bar_set(None)          # 後處理沒有百分比，改用來回跑的進度條
            self.lbl_status.configure(text=args[0])
        elif kind == "done":
            ok, fail, cancelled = args
            self._set_busy(False)
            self._bar_set(100 if not cancelled and fail == 0 else 0)
            if cancelled:
                msg = f"已取消。完成 {ok} 個。未完成的 .part 暫存檔保留在資料夾，下次同網址可接續下載。"
                self._log(msg, "warn")
            else:
                msg = f"全部結束：成功 {ok} 個" + (f"，失敗 {fail} 個（原因見紀錄）" if fail else "")
                self._log(msg, "ok" if fail == 0 else "warn")
            self.lbl_item.configure(text="待機")
            self.lbl_status.configure(text=msg)
            if not cancelled and fail == 0 and not getattr(self.job, "capture_urls", []):
                # 全部成功：清空網址區與紀錄，準備貼下一批（有失敗或取消時保留，才看得到原因、也能重試）
                self.url_text.delete("1.0", "end")
                self._clear_log()
            # 抖音、小紅書、TikTok 的解析器常因網站改版或要求登入而失敗，但瀏覽器能播，就能用「從瀏覽器擷取」抓下來
            retry = list(getattr(self.job, "capture_urls", [])) if not cancelled else []
            if retry and messagebox.askyesno(APP_NAME, "下面的網址下載失敗：\n\n" + "\n".join(retry) +
                                             "\n\n這類網站常常可以改用「從瀏覽器擷取」：瀏覽器能播，就能抓。要現在開啟嗎？"):
                self._capture(retry[0])
        elif kind == "probe":
            info, text = args
            self.btn_probe.configure(state="normal" if not self.busy else "disabled")
            self._log(text, "probe" if info else "error")
        elif kind == "pip_done":
            rc = args[0]
            self.btn_update.configure(state="normal" if not self.busy else "disabled")
            if rc == 0:
                self._log("下載核心已更新。請關閉並重新開啟本程式，新版本才會生效。", "ok")
                if yt_dlp is None:
                    messagebox.showinfo(APP_NAME, "安裝完成，請重新開啟本程式。")
            else:
                self._log(f"更新失敗（pip 結束代碼 {rc}），請看上方訊息。", "error")

    # ── 小工具 ───────────────────────────────────────────────────────────
    def _bar_set(self, pct: float | None) -> None:
        if pct is None:
            if str(self.bar.cget("mode")) != "indeterminate":
                self.bar.configure(mode="indeterminate")
                self.bar.start(15)
        else:
            if str(self.bar.cget("mode")) == "indeterminate":
                self.bar.stop()
                self.bar.configure(mode="determinate")
            self.bar["value"] = max(0.0, min(100.0, pct))

    def _log(self, text: str, tag: str = "info") -> None:
        self.log.configure(state="normal")
        stamp = datetime.now().strftime("%H:%M:%S")
        for i, line in enumerate(str(text).splitlines() or [""]):
            prefix = f"[{stamp}] " if i == 0 else " " * 11
            self.log.insert("end", prefix + line + "\n", tag)
        self.log.see("end")
        self.log.configure(state="disabled")

    def _clear_log(self) -> None:
        self.log.configure(state="normal")
        self.log.delete("1.0", "end")
        self.log.configure(state="disabled")


def _enable_dpi_awareness() -> None:
    """高解析度螢幕上不開這個，Windows 會把整個視窗點陣放大而變模糊。"""
    if sys.platform != "win32":
        return
    try:
        ctypes.windll.shcore.SetProcessDpiAwareness(1)
    except Exception:
        try:
            ctypes.windll.user32.SetProcessDPIAware()
        except Exception:
            pass


def main() -> None:
    _enable_dpi_awareness()
    App().mainloop()


if __name__ == "__main__":
    main()
