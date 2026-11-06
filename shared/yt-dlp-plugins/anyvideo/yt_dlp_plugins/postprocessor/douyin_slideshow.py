"""yt-dlp plugin: DouyinSlideshow —— 抖音「圖文」貼文轉成 mp4。

抖音的圖文貼文（一組照片加背景音樂）沒有影片檔：yt-dlp 的抖音解析器只認影片串流與音樂，
圖文貼文解析出來只剩音樂，所以下載到的是 mp3／m4a。這個後處理器在下載完成後檢查：
抖音、而且只拿到聲音 → 向抖音的 aweme detail 取 images，把圖片依序、平均分配到音樂長度，
和音樂一起合成 H.264 + AAC 的 mp4，取代原本的聲音檔。

Python 版直接載入這個檔案；Qt 版與 C# 版用下面的參數啟用（放在 EnsureH264 之前）：

    --plugin-dirs <程式目錄>/yt-dlp-plugins
    --use-postprocessor DouyinSlideshow

不是抖音、或抖音貼文不是圖文（沒有 images）就什麼都不做，所以影片模式永遠可以帶著它。
僅音訊模式不要啟用：那時使用者要的就是音樂。
"""
import json
import os
import re
import shutil
import tempfile

from yt_dlp.networking import Request
from yt_dlp.postprocessor.ffmpeg import FFmpegPostProcessor, FFmpegPostProcessorError
from yt_dlp.utils import PostProcessingError

_DETAIL_URL = "https://www.douyin.com/aweme/v1/web/aweme/detail/?aweme_id="
_AUDIO_EXTS = {"mp3", "m4a", "aac", "opus", "ogg", "wav", "flac"}
_FPS = 30
_MAX_SIDE = 4096          # 畫布最長邊上限（H.264 與常見播放器的安全範圍）
_MIN_SECONDS = 1.0        # 音樂太短（或查不到長度）時，每張圖至少停留的秒數


def _sniff_ext(data: bytes) -> str:
    if data[:3] == b"\xff\xd8\xff":
        return "jpg"
    if data[:8] == b"\x89PNG\r\n\x1a\n":
        return "png"
    if data[:4] == b"RIFF" and data[8:12] == b"WEBP":
        return "webp"
    if data[4:12] in (b"ftypheic", b"ftypheix", b"ftypmif1", b"ftypavif"):
        return "heic"
    return ""


def _rank(url: str) -> tuple[int, int]:
    """網址的優先順序：ffmpeg 解得開的格式（jpeg、webp、png）優先，heic／avif 排最後；同樣可用的取尺寸上限最大的。
    抖音的圖片網址靠 ~tplv-…:寬:高:… 指定尺寸（例如 3000:3000），~noop 是不處理的原檔；沒有尺寸的視為最小。"""
    low = url.lower()
    if any(b in low for b in ("heic", "heif", "avif")):
        return 0, 0
    if "~noop" in low:
        return 1, 10 ** 6
    m = re.search(r"~tplv-[^?]*?:(\d+):(\d+)", low)
    return 1, max(int(m.group(1)), int(m.group(2))) if m else 0


def _pick_url(image: dict) -> str:
    urls = [u for u in (image.get("url_list") or image.get("download_url_list") or []) if isinstance(u, str)]
    return max(urls, key=_rank, default="")           # 同分取清單裡最前面的


def image_urls(detail: dict) -> list[str]:
    """aweme detail 裡依序的圖片網址；不是圖文貼文就是空清單。"""
    images = detail.get("images") if isinstance(detail, dict) else None
    if not isinstance(images, list):
        return []
    return [u for u in (_pick_url(i) for i in images if isinstance(i, dict)) if u]


def canvas_size(w: int, h: int) -> tuple[int, int]:
    k = min(1.0, _MAX_SIDE / max(w, h))
    return max(2, int(w * k) // 2 * 2), max(2, int(h * k) // 2 * 2)


class DouyinSlideshowPP(FFmpegPostProcessor):
    def _get(self, url: str) -> bytes:
        return self._downloader.urlopen(Request(url, headers={"Referer": "https://www.douyin.com/"})).read()

    def _detail(self, aweme_id: str) -> dict:
        try:
            data = json.loads(self._get(_DETAIL_URL + aweme_id))
        except Exception as e:                       # 網路、403、不是 JSON：交給下面當作「不是圖文」
            self.report_warning(f"查詢抖音貼文內容失敗，保留音訊檔：{e}")
            return {}
        return data.get("aweme_detail") or {}

    def _audio_seconds(self, path: str) -> float:
        try:
            return float(self.get_metadata_object(path).get("format", {}).get("duration") or 0)
        except (FFmpegPostProcessorError, ValueError, OSError):
            return 0.0

    def _canvas(self, first_image: str) -> tuple[int, int]:
        """畫布大小 = 第一張圖的實際尺寸（長寬取偶數，最長邊不超過 _MAX_SIDE），不縮小高解析的圖；讀不到尺寸用 1080x1920。"""
        try:
            st = next(s for s in self.get_metadata_object(first_image).get("streams", []) if s.get("width"))
            return canvas_size(int(st["width"]), int(st["height"]))
        except (StopIteration, FFmpegPostProcessorError, KeyError, ValueError, OSError):
            return 1080, 1920

    @FFmpegPostProcessor._restrict_to(images=False)
    def run(self, info):
        path = info.get("filepath")
        if (info.get("extractor_key") != "Douyin" or not path or not os.path.exists(path)
                or (info.get("ext") or "").lower() not in _AUDIO_EXTS):
            return [], info

        urls = image_urls(self._detail(str(info.get("id") or "")))
        if not urls:
            return [], info

        self.to_screen(f"抖音圖文貼文：{len(urls)} 張圖片 + 音樂，合成 mp4")
        work = tempfile.mkdtemp(prefix="avd-slideshow-")
        try:
            files = []
            for n, u in enumerate(urls, 1):
                data = self._get(u)
                ext = _sniff_ext(data) or "jpg"
                f = os.path.join(work, f"{n:03d}.{ext}")
                with open(f, "wb") as fh:
                    fh.write(data)
                files.append(f)

            total = self._audio_seconds(path)
            each = max(total / len(files), _MIN_SECONDS) if total else 3.0
            w, h = self._canvas(files[0])
            # 每張圖一個輸入（-loop 1 -t 秒數），縮放補黑邊成同樣大小後用 concat 濾鏡接起來。
            # 不用 concat demuxer：它對圖片檔只會顯示第一張，後面的圖會解碼失敗。
            fit = (f"scale={w}:{h}:force_original_aspect_ratio=decrease,"
                   f"pad={w}:{h}:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps={_FPS},format=yuv420p")
            graph = "".join(f"[{i}:v]{fit}[v{i}];" for i in range(len(files)))
            graph += "".join(f"[v{i}]" for i in range(len(files))) + f"concat=n={len(files)}:v=1:a=0[v]"
            dst = os.path.splitext(path)[0] + ".mp4"
            tmp = os.path.join(work, "out.mp4")
            opts = ["-filter_complex", graph, "-map", "[v]", "-map", f"{len(files)}:a:0",
                    "-c:v", "libx264", "-preset", "fast", "-crf", "20",
                    "-c:a", "aac", "-b:a", "192k", *(["-t", f"{total:.3f}"] if total else ["-shortest"]),
                    "-movflags", "+faststart"]
            inputs = [(f, ["-loop", "1", "-t", f"{each:.3f}"]) for f in files] + [(path, [])]
            try:
                self.real_run_ffmpeg(inputs, [(tmp, opts)])
            except FFmpegPostProcessorError as e:
                raise PostProcessingError(f"合成圖文影片失敗：{e.msg}")
            shutil.move(tmp, dst)
        finally:
            shutil.rmtree(work, ignore_errors=True)

        info["filepath"], info["ext"] = dst, "mp4"
        return [path], info                          # 舊的聲音檔交給 yt-dlp 刪除
