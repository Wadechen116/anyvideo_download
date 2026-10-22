"""從瀏覽器擷取 —— 網站頁面解析不了、但瀏覽器能播時使用（例如 Meta Reels、抖音、小紅書）。

開一個 Chrome／Edge（偵錯模式、獨立設定檔），使用者照常播放，程式透過 Chrome DevTools Protocol 監聽網路請求，找出播放器
實際載入的影音網址（m3u8、mpd、mp4…），用 yt-dlp 下載（以 Chrome 身分連線，否則 fbcdn 這類 CDN 會限速），再用 ffmpeg 串流複製
合併。有 DRM 保護的內容抓不到可用的資料。

自動挑最高畫質與最高音質：
  1. 先問頁面裡正在播放的 <video>：長度幾秒。Facebook 會預先載入下一支影片，要靠長度把它們分開。
  2. 把偵測到的檔案（最多 12 個）全部下載，用 ffprobe 量每個檔案的解析度、位元率、長度。
  3. 只留長度和正在播放的影片最接近的那一組（差距容許 0.15 秒，不是 ±0.6 秒：實測 10.4 秒的影片旁邊有 10.0 秒的另一支）。
  4. 影像取解析度最大的（同解析度取位元率最高），聲音取位元率最高的，合併成 mp4。
限制：只能挑「播放器實際請求過的」畫質。播放器的自動畫質切換一開始常常先請求低畫質；要更高畫質，先在網站播放器裡把畫質調高再擷取。

規則與 qt/src/capture.cpp、csharp/Capture.cs 相同（tests/capture_flow.py 三個版本跑同一個離線端對端測試）。
"""
from __future__ import annotations

import base64
import json
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable

_NO_WINDOW = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
MAX_FILES = 12          # 最多下載幾個偵測到的檔案


# ─── 找出影音網址 ──────────────────────────────────────────────────────────

@dataclass
class Candidate:
    url: str            # 已去掉 bytestart／byteend（Meta 把一個檔切成很多段請求）
    mime: str = ""
    referer: str = ""
    kind: str = ""      # stream（m3u8／mpd）、video、audio、media
    size: int = -1      # 未知為 -1
    seq: int = 0        # 第幾個被偵測到（越小越早）


def find_browser() -> str | None:
    """Chrome → Edge；找不到回傳 None。"""
    roots = [os.environ.get("ProgramFiles"), os.environ.get("ProgramFiles(x86)"), os.environ.get("LOCALAPPDATA")]
    for rel in ("Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe"):
        for root in roots:
            if root and (Path(root) / rel).exists():
                return str(Path(root) / rel)
    return None


def candidate_kind(url: str, mime: str) -> str:
    """是影音就回傳種類，不是回傳空字串。"""
    path = urllib.parse.urlsplit(url).path.lower()
    m = mime.lower()
    if path.endswith(".m3u8") or "mpegurl" in m:
        return "stream"
    if path.endswith(".mpd") or "dash+xml" in m:
        return "stream"
    # 切片檔單獨拿沒有用：.ts／.m4s 是串流的一小段，要靠 m3u8／mpd 才能組回來
    if path.endswith(".ts") or path.endswith(".m4s") or "mp2t" in m:
        return ""
    if m.startswith("audio/"):
        return "audio"
    if m.startswith("video/"):
        return "video"
    if path.endswith((".mp4", ".webm", ".m4a")):
        return "media"
    return ""


def clean_media_url(url: str) -> str:
    """Meta 把同一個檔拆成很多 bytestart／byteend 範圍請求；拿掉才是完整檔案。"""
    sp = urllib.parse.urlsplit(url)
    q = [(k, v) for k, v in urllib.parse.parse_qsl(sp.query, keep_blank_values=True) if k not in ("bytestart", "byteend")]
    return urllib.parse.urlunsplit(sp._replace(query=urllib.parse.urlencode(q, safe="%:/=+,;@*!$'()~-._")))


def url_duration(url: str) -> float:
    """網址自己帶的長度（Facebook 的 efg 參數有 duration_s）；沒有回傳 -1。"""
    efg = dict(urllib.parse.parse_qsl(urllib.parse.urlsplit(url).query)).get("efg")
    if not efg:
        return -1
    try:
        data = json.loads(base64.urlsafe_b64decode(efg + "=" * (-len(efg) % 4)))
        return float(data["duration_s"]) if "duration_s" in data else -1
    except (ValueError, KeyError, TypeError):
        return -1


def _header(headers: dict, name: str) -> str:
    for k, v in (headers or {}).items():
        if k.lower() == name.lower():
            return str(v)
    return ""


# ─── DevTools 連線 ─────────────────────────────────────────────────────────

class CaptureSession:
    """單一瀏覽器的 DevTools 連線，只負責找出影音網址。回呼都在背景執行緒呼叫，介面要自己轉回主執行緒。"""

    def __init__(self, on_found: Callable[[Candidate], None], on_status: Callable[[str], None], profile: str | None = None):
        self.on_found, self.on_status = on_found, on_status
        # AVD_CAPTURE_PROFILE：測試用，改用另一個設定檔資料夾（不碰你平常的擷取設定檔，也不會被它的舊程序鎖住）
        self.profile = profile or os.environ.get("AVD_CAPTURE_PROFILE") or str(
            Path(os.environ.get("APPDATA") or Path.home()) / "anyvideo_download" / "capture-profile")
        self.seen: dict[str, Candidate] = {}
        self._ws = None
        self._thread: threading.Thread | None = None
        self._stop = threading.Event()
        self._send_lock = threading.Lock()
        self._next_id = 1
        self._start_url = ""
        self._navigated = False
        self._sessions: dict[str, str] = {}          # sessionId -> 目標類型（page、iframe、worker…）
        self._get_targets_id = 0
        self._pending_eval: set[int] = set()
        self._videos: list[list[float]] = []
        self._eval_done = threading.Event()
        self.port = 0

    def start(self, start_url: str = "") -> None:
        browser = find_browser()
        if not browser:
            raise RuntimeError("找不到 Chrome 或 Edge。")
        import socket
        with socket.socket() as s:                    # 向系統要一個沒人用的埠
            s.bind(("127.0.0.1", 0))
            self.port = s.getsockname()[1]
        self._start_url = start_url
        # 獨立的設定檔：不影響你平常用的瀏覽器（Chrome 也規定偵錯模式必須用獨立設定檔）。
        # 要看登入限定的內容，在這個視窗登入一次，下次還在。
        args = [browser, f"--remote-debugging-port={self.port}", f"--user-data-dir={self.profile}",
                "--no-first-run", "--no-default-browser-check", "about:blank"]
        flags = (subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP) if sys.platform == "win32" else 0
        try:
            subprocess.Popen(args, creationflags=flags, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            raise RuntimeError(f"無法啟動瀏覽器：{browser}（{e}）") from e
        self.on_status("正在開啟瀏覽器…")
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def stop(self) -> None:
        """結束監聽（瀏覽器視窗不關）。"""
        self._stop.set()
        ws = self._ws
        if ws is not None:
            try:
                ws.close()
            except Exception:  # noqa: BLE001
                pass

    # ── 背景執行緒 ──
    def _send(self, method: str, params: dict | None = None, session_id: str | None = None) -> int:
        with self._send_lock:
            msg = {"id": self._next_id, "method": method, "params": params or {}}
            if session_id:
                msg["sessionId"] = session_id
            self._next_id += 1
            if self._ws is not None:
                self._ws.send(json.dumps(msg))
            return msg["id"]

    def _run(self) -> None:
        from websockets.exceptions import ConnectionClosed
        from websockets.sync.client import connect
        ws_url = ""
        for _ in range(50):
            if self._stop.is_set():
                return
            try:
                with urllib.request.urlopen(f"http://127.0.0.1:{self.port}/json/version", timeout=2) as r:
                    ws_url = json.load(r).get("webSocketDebuggerUrl", "")
                if ws_url:
                    break
            except (OSError, ValueError):
                pass
            time.sleep(0.4)
        if not ws_url:
            self.on_status("連不上瀏覽器的偵錯埠。請關閉所有用同一個設定檔開啟的瀏覽器視窗後再試。")
            return
        try:
            with connect(ws_url, max_size=None) as ws:
                self._ws = ws
                self.on_status("已連上瀏覽器：請在瀏覽器裡開啟並播放影片")
                self._send("Target.setAutoAttach", {"autoAttach": True, "waitForDebuggerOnStart": False, "flatten": True})
                self._get_targets_id = self._send("Target.getTargets")
                while not self._stop.is_set():
                    try:
                        raw = ws.recv(timeout=0.5)
                    except TimeoutError:
                        continue
                    self._on_message(raw)
        except ConnectionClosed:
            pass
        except OSError:
            pass
        finally:
            self._ws = None
            if not self._stop.is_set():
                self.on_status("瀏覽器已關閉，擷取結束")

    def _on_message(self, raw) -> None:
        # 播放影片時瀏覽器會每秒送上千則 dataReceived 之類的事件；不是要用的就連 JSON 都不解析
        if not ('"id":' in raw or 'Network.responseReceived"' in raw or "Target.attachedToTarget" in raw):
            return
        m = json.loads(raw)
        rid = m.get("id")
        if rid is not None and rid == self._get_targets_id:
            for t in m.get("result", {}).get("targetInfos", []):
                if t.get("type") == "page":
                    self._send("Target.attachToTarget", {"targetId": t["targetId"], "flatten": True})
            return
        if rid is not None and rid in self._pending_eval:                       # Runtime.evaluate 的結果
            self._pending_eval.discard(rid)
            value = m.get("result", {}).get("result", {}).get("value", "")
            try:
                for row in json.loads(value or "[]"):
                    if len(row) >= 5:
                        self._videos.append([float(x) for x in row])
            except (ValueError, TypeError):
                pass
            if not self._pending_eval:
                self._eval_done.set()
            return
        method, params = m.get("method", ""), m.get("params", {})
        if method == "Target.attachedToTarget":
            sid = params["sessionId"]
            ttype = params.get("targetInfo", {}).get("type", "")
            self._sessions[sid] = ttype
            self._send("Network.enable", {}, sid)
            # 網頁裡的 worker／iframe 是另外的目標，Meta 的影片就是 worker 去抓的；每一層都要接著監聽
            self._send("Target.setAutoAttach", {"autoAttach": True, "waitForDebuggerOnStart": False, "flatten": True}, sid)
            if not self._navigated and self._start_url and ttype == "page":
                # 連上之後才導向，避免頁面在開始監聽前就載完而漏掉請求
                self._navigated = True
                self._send("Page.navigate", {"url": self._start_url}, sid)
        elif method == "Network.responseReceived":
            resp = params.get("response", {})
            kind = candidate_kind(resp.get("url", ""), resp.get("mimeType", ""))
            if not kind or resp.get("status", 0) >= 400:
                return
            c = Candidate(url=clean_media_url(resp["url"]), mime=resp.get("mimeType", ""), kind=kind,
                          referer=_header(resp.get("requestHeaders"), "Referer"))
            rng = _header(resp.get("headers"), "Content-Range")            # 範圍請求的 Content-Range 末尾是整個檔案的大小
            size = rng.rsplit("/", 1)[-1] if "/" in rng else _header(resp.get("headers"), "Content-Length")
            c.size = int(size) if size.isdigit() and int(size) > 0 else -1
            old = self.seen.get(c.url)
            if old is None:
                c.seq = len(self.seen)
                self.seen[c.url] = c
                self.on_found(c)
            elif c.size > old.size or (not old.referer and c.referer):
                old.size = max(old.size, c.size)
                old.referer = old.referer or c.referer
                self.on_found(old)

    def query_playing_video(self, timeout: float = 3.0) -> float:
        """問頁面裡正在播放的 <video>：長度幾秒。正在播放的那支：沒暫停、已經播了一點的；沒有的話取播放進度最多的。
        長度不明（直播等）或讀不到回傳 -1。"""
        expression = ("JSON.stringify([...document.querySelectorAll('video')].map(v=>[Number.isFinite(v.duration)?v.duration:-1,"
                      "v.paused?1:0,v.currentTime,v.videoWidth,v.videoHeight]))")
        self._videos = []
        self._eval_done.clear()
        self._pending_eval = set()
        for sid, ttype in list(self._sessions.items()):
            if ttype in ("page", "iframe"):
                try:
                    self._pending_eval.add(self._next_id)
                    self._send("Runtime.evaluate", {"expression": expression, "returnByValue": True}, sid)
                except Exception:  # noqa: BLE001
                    return -1
        if not self._pending_eval:
            return -1
        self._eval_done.wait(timeout)                  # 有頁面沒回應也不能一直等
        duration, best = -1.0, -1.0
        for v in list(self._videos):
            playing = v[1] == 0 and v[2] > 0.3
            score = (1e6 if playing else 0) + v[2]
            if v[0] > 0 and score > best:
                best, duration = score, v[0]
        return duration


# ─── 下載、挑選、合併 ──────────────────────────────────────────────────────

def pick_candidates(candidates: list[Candidate], target_duration: float) -> list[Candidate]:
    """有串流（m3u8／mpd）就用串流（yt-dlp 自己會挑最高的）；否則把偵測到的檔案（最多 12 個）全部抓下來，
    網址已經帶長度的先把明顯是別支影片的略過，少抓一些。"""
    ordered = sorted(candidates, key=lambda c: c.seq)
    for c in ordered:
        if c.kind == "stream":
            return [c]
    if target_duration > 0:
        same = [c for c in ordered if (d := url_duration(c.url)) < 0 or abs(d - target_duration) <= 1.5]
        if same:
            ordered = same
    return ordered[:MAX_FILES]


def have_impersonation() -> bool:
    try:
        import curl_cffi  # noqa: F401
        return True
    except ImportError:
        return False


def download_one(c: Candidate, out_template: str, ffmpeg_dir: str | None, log: Callable[[str], None],
                 progress: Callable[[float], None] | None = None, cancel: threading.Event | None = None) -> str | None:
    """用 yt-dlp 下載一個檔案，回傳檔案路徑。先以 Chrome 身分連線（fbcdn 這類 CDN 對 yt-dlp 預設的連線特徵會限速：實測同一個
    0.6 MB 檔案，預設 86 秒、偽裝成 Chrome 2.5 秒）；沒裝 curl_cffi 或很快就失敗時，改用預設方式再試一次。"""
    import yt_dlp
    from yt_dlp.utils import DownloadCancelled

    def attempt(impersonate: bool) -> str | None:
        opts = {"outtmpl": out_template, "quiet": True, "no_warnings": True, "noprogress": True, "noplaylist": True,
                "retries": 10, "fragment_retries": 10, "socket_timeout": 30, "windowsfilenames": True,
                "merge_output_format": "mp4/mkv"}
        if ffmpeg_dir:
            opts["ffmpeg_location"] = ffmpeg_dir
        if c.referer:
            opts["http_headers"] = {"Referer": c.referer}
        if impersonate:
            from yt_dlp.networking.impersonate import ImpersonateTarget
            opts["impersonate"] = ImpersonateTarget.from_str("chrome")

        def hook(d: dict) -> None:
            if cancel is not None and cancel.is_set():
                raise DownloadCancelled("使用者停止")
            if d.get("status") == "downloading" and progress:
                total = d.get("total_bytes") or d.get("total_bytes_estimate")
                if total:
                    progress((d.get("downloaded_bytes") or 0) * 100 / total)
        opts["progress_hooks"] = [hook]
        with yt_dlp.YoutubeDL(opts) as ydl:
            info = ydl.extract_info(c.url, download=True)
            reqs = (info or {}).get("requested_downloads") or []
            return reqs[0].get("filepath") if reqs else (ydl.prepare_filename(info) if info else None)

    started = time.monotonic()
    if have_impersonation():
        try:
            return attempt(True)
        except DownloadCancelled:
            raise
        except Exception as e:  # noqa: BLE001
            if time.monotonic() - started >= 15:
                log(f"下載失敗：{e}")
                return None
            log("以 Chrome 身分連線失敗，改用預設方式重試這一項…")
    try:
        return attempt(False)
    except DownloadCancelled:
        raise
    except Exception as e:  # noqa: BLE001
        log(f"下載失敗：{e}")
        return None


@dataclass
class FileInfo:
    path: str
    video: bool = False
    audio: bool = False
    w: int = 0
    h: int = 0
    dur: float = -1
    size: int = 0

    @property
    def pixels(self) -> float:
        return float(self.w) * self.h

    @property
    def bitrate(self) -> float:
        return self.size * 8.0 / self.dur if self.dur > 0 else 0.0


def probe_file(path: str, ffmpeg_dir: str | None) -> FileInfo:
    info = FileInfo(path, size=os.path.getsize(path), video=True)
    if not ffmpeg_dir:
        return info
    r = subprocess.run([str(Path(ffmpeg_dir) / "ffprobe"), "-v", "error", "-print_format", "json", "-show_entries",
                        "stream=codec_type,width,height:format=duration", path],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)
    try:
        data = json.loads(r.stdout or "{}")
    except ValueError:
        return info
    info.video = info.audio = False
    for s in data.get("streams", []):
        if s.get("codec_type") == "video" and not info.video:
            info.video, info.w, info.h = True, int(s.get("width", 0)), int(s.get("height", 0))
        elif s.get("codec_type") == "audio":
            info.audio = True
    try:
        info.dur = float(data.get("format", {}).get("duration"))
    except (TypeError, ValueError):
        info.dur = -1
    return info


def choose_and_merge(files: list[str], ffmpeg_dir: str | None, target_duration: float, out_dir: str, stamp: str,
                     log: Callable[[str], None]) -> str | None:
    """下載到的檔案裡挑出「正在播放的那支」最好的影像與聲音，合併成 mp4。回傳結果路徑。"""
    files = [f for f in files if f and os.path.exists(f)]
    if not files:
        log("沒有下載到任何檔案，原因見上方紀錄。")
        return None
    infos = [probe_file(f, ffmpeg_dir) for f in files]
    for i in infos:
        kinds = ("影像 %d×%d " % (i.w, i.h) if i.video else "") + ("聲音" if i.audio else "")
        log(f"  {os.path.basename(i.path)}：{kinds} {i.dur:.2f} 秒 {i.size // 1024} KB")
    pool = infos
    if target_duration > 0:
        # 只留「正在播放的那支」：長度和頁面上的 <video> 最接近的那一組。同一支影片的各種畫質、影像與聲音，長度只差幾百分之一秒；
        # 預先載入的下一支影片長度不同，即使只差 0.4 秒也不能混進來。所以先找出最接近的差距，只留和它差不到 0.15 秒的。
        diffs = [abs(i.dur - target_duration) for i in infos if i.dur > 0]
        best_diff = min(diffs) if diffs else 1e9
        same = [i for i in infos if i.dur > 0 and abs(i.dur - target_duration) <= best_diff + 0.15] \
            if best_diff <= max(0.6, target_duration * 0.05) else []
        if same:
            pool = same
        log(f"長度最接近正在播放影片的檔案：{len(same)}／{len(infos)} 個")
    # 最高畫質＝解析度最大、同解析度取位元率最高；最高音質＝位元率最高。
    def better(a: FileInfo, b: FileInfo) -> bool:
        return a.pixels > b.pixels if a.pixels != b.pixels else a.bitrate > b.bitrate
    whole = best_v = best_a = None
    for i in pool:
        if i.video and i.audio:
            if whole is None or better(i, whole):
                whole = i
        elif i.video:
            if best_v is None or better(i, best_v):
                best_v = i
        elif i.audio:
            if best_a is None or i.bitrate > best_a.bitrate:
                best_a = i
    # 影像與聲音分開的組合，和影像聲音在一起的檔案比：解析度較高（或一樣高）的組合勝出，因為聲音也是挑最高的。
    pair = best_v is not None and best_a is not None and (whole is None or best_v.pixels >= whole.pixels)
    keep: str
    if pair:
        log(f"選用：影像 {best_v.w}×{best_v.h}、聲音 {int(best_a.bitrate / 1000)} kbps")
    elif whole is not None:
        log(f"選用：影像聲音在一起的檔案 {whole.w}×{whole.h}")
        for f in files:
            if f != whole.path:
                _remove(f)
        log("完成：" + whole.path)
        return whole.path
    elif best_v is not None:
        log(f"只找到影像（{best_v.w}×{best_v.h}），沒有找到聲音")
        for f in files:
            if f != best_v.path:
                _remove(f)
        log("完成：" + best_v.path)
        return best_v.path
    else:
        log("完成：" + files[0])
        return files[0]
    if not ffmpeg_dir:
        log("找不到 ffmpeg，無法合併影像與聲音；檔案留在儲存位置。")
        return best_v.path
    out = str(Path(out_dir) / f"擷取_{stamp}.mp4")
    r = subprocess.run([str(Path(ffmpeg_dir) / "ffmpeg"), "-y", "-loglevel", "error", "-i", best_v.path, "-i", best_a.path,
                        "-map", "0:v:0", "-map", "1:a:0", "-c", "copy", "-shortest", "-movflags", "+faststart", out],
                       capture_output=True, text=True, encoding="utf-8", errors="replace", creationflags=_NO_WINDOW)
    if r.returncode != 0 or not os.path.exists(out):
        log("合併失敗：" + (r.stderr or "").strip()[-300:])
        return best_v.path
    for f in files:
        _remove(f)
    log("完成：" + out)
    return out


def _remove(path: str) -> None:
    try:
        os.remove(path)
    except OSError:
        pass


def download_and_merge(candidates: list[Candidate], target_duration: float, out_dir: str, ffmpeg_dir: str | None,
                       log: Callable[[str], None], progress: Callable[[float], None] | None = None,
                       status: Callable[[str], None] | None = None, cancel: threading.Event | None = None) -> str | None:
    """偵測到的影音 → 挑出要下載的 → 下載 → 挑最好的影像與聲音 → 合併。回傳結果路徑；停止或失敗回傳 None。"""
    from yt_dlp.utils import DownloadCancelled
    queue = pick_candidates(candidates, target_duration)
    if not queue:
        return None
    Path(out_dir).mkdir(parents=True, exist_ok=True)
    stamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    files: list[str] = []
    for i, c in enumerate(queue, 1):
        if cancel is not None and cancel.is_set():
            return None
        if status:
            status(f"下載第 {i}/{len(queue)} 項…")
        size = "大小未知" if c.size < 0 else f"{c.size / 1048576:.1f} MB"
        log(f"第 {i}/{len(queue)} 項：[{c.kind}] {size} {c.url[:110]}")
        t0 = time.monotonic()
        try:
            path = download_one(c, str(Path(out_dir) / f"擷取_{stamp}_{i}.%(ext)s"), ffmpeg_dir, log, progress, cancel)
        except DownloadCancelled:
            return None
        if path:
            files.append(path)
            log(f"第 {i} 項完成（{int(time.monotonic() - t0)} 秒）")
    if cancel is not None and cancel.is_set():
        return None
    if status:
        status("挑選最高畫質與音質，合併中…")
    return choose_and_merge(files, ffmpeg_dir, target_duration, out_dir, stamp, log)


def run_headless(url: str, out_dir: str, wait_seconds: float = 4, profile: str | None = None,
                 log: Callable[[str], None] = print, ffmpeg_dir: str | None = None) -> str | None:
    """不開介面：開瀏覽器載入網址、等影音出現、再等幾秒讓影像與聲音都出現、下載並合併。回傳結果路徑。給測試用。"""
    import shutil
    if ffmpeg_dir is None:
        exe = shutil.which("ffmpeg")
        ffmpeg_dir = str(Path(exe).parent) if exe else None
    session = CaptureSession(lambda c: log(f"偵測到 [{c.kind}] {c.mime} {c.url[:90]}"), log, profile)
    try:
        session.start(url)
    except RuntimeError as e:
        log(str(e))
        return None
    try:
        deadline = time.monotonic() + 45
        while not session.seen and time.monotonic() < deadline:
            time.sleep(0.2)
        if not session.seen:
            log("沒有偵測到任何影音。")
            return None
        time.sleep(wait_seconds)
        target = session.query_playing_video()
        log(f"正在播放的影片長度：{target:.1f} 秒" if target > 0 else "讀不到正在播放的影片長度，改用偵測順序判斷")
        return download_and_merge(list(session.seen.values()), target, out_dir, ffmpeg_dir, log)
    finally:
        session.stop()


# ─── 視窗 ─────────────────────────────────────────────────────────────────

def _make_dialog_class():
    import queue
    import tkinter as tk
    from tkinter import ttk

    class CaptureDialog(tk.Toplevel):
        """「從瀏覽器擷取」視窗：開瀏覽器 → 你播放 → 顯示偵測到幾個影音 → 一鍵下載（自動挑最高畫質與音質）。"""

        def __init__(self, master, out_dir: str, ffmpeg_dir: str | None, start_url: str = ""):
            super().__init__(master)
            self.title("從瀏覽器擷取影片")
            self.geometry("760x520")
            self.out_dir, self.ffmpeg_dir = out_dir, ffmpeg_dir
            self.q: queue.Queue = queue.Queue()
            self.session: CaptureSession | None = None
            self.candidates: dict[str, Candidate] = {}
            self.browser_open = False
            self.job: threading.Thread | None = None
            self.cancel = threading.Event()
            self.result: str | None = None

            f = ttk.Frame(self, padding=10)
            f.pack(fill="both", expand=True)
            hint = ttk.Label(f, justify="left", wraplength=720, text=(
                "網站解析不了、但瀏覽器能播時使用：\n"
                "① 按「開啟瀏覽器」。 ② 在開出來的瀏覽器裡播放影片（需要登入的在那裡登入一次，之後會記住）。\n"
                "③ 這裡顯示「偵測到影音」後，按「下載擷取到的影片」。程式會自動挑出最高畫質的影像與最高音質的聲音並合併。\n"
                "有 DRM 保護的內容（Netflix 等）抓不到可用的資料。"))
            hint.pack(anchor="w")
            row = ttk.Frame(f)
            row.pack(fill="x", pady=(8, 0))
            self.var_url = tk.StringVar(value=start_url)
            ttk.Entry(row, textvariable=self.var_url).pack(side="left", fill="x", expand=True)
            self.btn_open = ttk.Button(row, text="開啟瀏覽器", command=self._open_browser)
            self.btn_open.pack(side="left", padx=(6, 0))
            self.lbl_found = ttk.Label(f, text="尚未偵測到影音", font=("TkDefaultFont", 10, "bold"))
            self.lbl_found.pack(anchor="w", pady=(8, 0))
            row2 = ttk.Frame(f)
            row2.pack(fill="x", pady=(6, 0))
            self.btn_dl = ttk.Button(row2, text="下載擷取到的影片", command=self._download, state="disabled")
            self.btn_dl.pack(side="left")
            self.btn_stop = ttk.Button(row2, text="停止", command=self._stop, state="disabled")
            self.btn_stop.pack(side="left", padx=6)
            ttk.Button(row2, text="關閉", command=self._close).pack(side="right")
            self.bar = ttk.Progressbar(f, maximum=100)
            self.bar.pack(fill="x", pady=(8, 2))
            self.lbl_status = ttk.Label(f, text="", wraplength=720)
            self.lbl_status.pack(anchor="w")
            self.log = tk.Text(f, height=10, wrap="word", state="disabled")
            self.log.pack(fill="both", expand=True, pady=(6, 0))
            self.protocol("WM_DELETE_WINDOW", self._close)
            self.after(100, self._poll)

        # ── 動作 ──
        def _log(self, text: str) -> None:
            self.log.configure(state="normal")
            self.log.insert("end", datetime.now().strftime("%H:%M:%S  ") + text + "\n")
            self.log.see("end")
            self.log.configure(state="disabled")

        def _buttons(self) -> None:
            running = self.job is not None and self.job.is_alive()
            self.btn_dl.configure(state="normal" if self.candidates and not running else "disabled")
            self.btn_stop.configure(state="normal" if running or self.browser_open else "disabled")
            self.btn_open.configure(state="disabled" if self.browser_open else "normal")

        def _open_browser(self) -> None:
            self.session = CaptureSession(lambda c: self.q.put(("found", c)), lambda t: self.q.put(("status", t)))
            try:
                self.session.start(self.var_url.get().strip())
            except RuntimeError as e:
                self.lbl_status.configure(text=str(e))
                self._log(str(e))
                return
            self.browser_open = True
            self._buttons()

        def _download(self) -> None:
            if self.job is not None and self.job.is_alive():
                return
            self.cancel.clear()
            self.btn_dl.configure(state="disabled")
            self.btn_stop.configure(state="normal")
            self.lbl_status.configure(text="讀取正在播放的影片…")
            self.result = None

            def work() -> None:
                # 先問瀏覽器：現在在播的是哪一支（長度幾秒）。Facebook 等網站會預先載入下一支，要靠長度把它們分開。
                target = self.session.query_playing_video() if self.session and self.browser_open else -1
                self.q.put(("log", f"正在播放的影片長度：{target:.1f} 秒" if target > 0 else "讀不到正在播放的影片長度，改用偵測順序判斷"))
                path = download_and_merge(list(self.candidates.values()), target, self.out_dir, self.ffmpeg_dir,
                                          lambda t: self.q.put(("log", t)), lambda p: self.q.put(("progress", p)),
                                          lambda t: self.q.put(("status", t)), self.cancel)
                self.q.put(("done", path))
            self.job = threading.Thread(target=work, daemon=True)
            self.job.start()

        def _stop(self) -> None:
            if self.job is not None and self.job.is_alive():
                self.cancel.set()
                return
            if self.session and self.browser_open:
                self.session.stop()
                self.browser_open = False
                self.lbl_status.configure(text="已停止擷取（瀏覽器視窗可自行關閉）")
            self._buttons()

        def _close(self) -> None:
            self.cancel.set()
            if self.session:
                self.session.stop()
            self.destroy()

        def _poll(self) -> None:
            try:
                while True:
                    kind, arg = self.q.get_nowait()
                    if kind == "found":
                        if arg.url not in self.candidates:
                            size = "大小未知" if arg.size < 0 else f"{arg.size / 1048576:.1f} MB"
                            self._log(f"偵測到 [{arg.kind}] {arg.mime} {size} {urllib.parse.urlsplit(arg.url).netloc}")
                        self.candidates[arg.url] = arg
                        self.lbl_found.configure(text=f"偵測到影音：{len(self.candidates)} 個項目")
                        self._buttons()
                    elif kind == "status":
                        self.lbl_status.configure(text=arg)
                    elif kind == "log":
                        self._log(arg)
                    elif kind == "progress":
                        self.bar["value"] = max(0.0, min(100.0, arg))
                    elif kind == "done":
                        self.result = arg
                        self.bar["value"] = 100 if arg else 0
                        self.lbl_status.configure(text=("完成：" + arg) if arg else
                                                  ("已停止。" if self.cancel.is_set() else "沒有下載到任何檔案，原因見下方紀錄。"))
                        self._buttons()
            except queue.Empty:
                pass
            if self.winfo_exists():
                self.after(100, self._poll)

    return CaptureDialog


def open_dialog(master, out_dir: str, ffmpeg_dir: str | None, start_url: str = ""):
    """開「從瀏覽器擷取」視窗（非互動式，立刻回傳）。"""
    return _make_dialog_class()(master, out_dir, ffmpeg_dir, start_url)
