"""下載核心的煙霧測試 —— 不開介面、不連任何外部網站。

做法：用 ffmpeg 產生一段 3 秒的測試影片（彩條 + 440 Hz 音），在 127.0.0.1 起一個
HTTP 服務，讓 DownloadJob 用正式的程式碼路徑去下載它，再轉成每一種輸出格式，
最後用 ffprobe 確認產出的檔案真的是那個格式、而且有影像／聲音。

執行（Windows）：
    py -3 tests\\smoke_test.py
"""
from __future__ import annotations

import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import anyvideo_download as av  # noqa: E402


def make_sample(dst: Path, vcodec: str = "libx264", size: str = "640x360") -> None:
    # AV1／VP9 編碼很慢，挑最快的檔位
    extra = {"libsvtav1": ["-preset", "12"], "libvpx-vp9": ["-deadline", "realtime", "-cpu-used", "8"]}.get(vcodec, [])
    audio = "libopus" if dst.suffix == ".webm" else "aac"
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-f", "lavfi", "-i", f"testsrc=duration=3:size={size}:rate=25",
         "-f", "lavfi", "-i", "sine=frequency=440:duration=3",
         "-c:v", vcodec, *extra, "-pix_fmt", "yuv420p", "-c:a", audio, "-shortest", str(dst)],
        check=True, env={**os.environ, "SVT_LOG": "1"})   # SVT_LOG=1：SVT-AV1 只印錯誤


def make_dash(src_mp4: Path, dst_dir: Path) -> None:
    """把同一段影片切成 DASH：影像與聲音是兩個獨立串流。

    真正的影音網站大多這樣提供，下載端得分別抓影像與聲音再合併 ——
    這條「合併」路徑是直接下載單一 .mp4 測不到的。
    """
    dst_dir.mkdir()
    # cwd 一定要設成輸出目錄：ffmpeg 的 dash 封裝器把分段檔寫在「目前工作目錄」，
    # 而不是 manifest 所在的目錄。沒設的話 manifest 旁邊一個分段都沒有，
    # 下載端只會回報「fragment 1 not found」。
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error", "-i", str(src_mp4.resolve()),
         "-map", "0:v", "-map", "0:a", "-c", "copy", "-f", "dash",
         "-adaptation_sets", "id=0,streams=v id=1,streams=a",
         "manifest.mpd"],
        check=True, cwd=dst_dir)


def make_mixed_dash(av1_mp4: Path, h264_mp4: Path, dst_dir: Path) -> None:
    """一份 manifest 裡同時有 AV1 360p 與 H.264 240p 兩種影像 —— 模擬 YouTube。

    用來確認「取捨」真的在挑格式：畫質優先要挑解析度較高的 AV1，相容優先要
    直接挑 H.264，不必重新編碼。
    """
    dst_dir.mkdir()
    subprocess.run(
        ["ffmpeg", "-y", "-loglevel", "error",
         "-i", str(av1_mp4.resolve()), "-i", str(h264_mp4.resolve()),
         "-map", "0:v", "-map", "1:v", "-map", "0:a", "-c", "copy", "-f", "dash",
         "-adaptation_sets", "id=0,streams=0 id=1,streams=1 id=2,streams=2",
         "manifest.mpd"],
        check=True, cwd=dst_dir)


def probe(path: Path) -> dict:
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_entries",
         "stream=codec_type,codec_name,height,disposition:format=format_name",
         "-of", "json", str(path)], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


class _QuietHandler(http.server.SimpleHTTPRequestHandler):
    def log_message(self, *args, **kwargs):   # 不要把每個請求都印出來
        pass


def serve(root: Path) -> tuple[http.server.ThreadingHTTPServer, str]:
    handler = functools.partial(_QuietHandler, directory=str(root))
    srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, f"http://127.0.0.1:{srv.server_address[1]}"


def run_case(name: str, settings: av.Settings, url: str,
             vcodec: str | None = None, height: int | None = None) -> tuple[bool, str]:
    """vcodec / height：產出檔案的影像必須是這個編碼、這個高度（None 表示不檢查）。"""
    posts: list = []
    job = av.DownloadJob([url], settings, av.find_ffmpeg(),
                         lambda kind, *a: posts.append((kind, a)), threading.Event())
    job.run()                                   # 直接在這個執行緒跑，不另開
    done = [a for k, a in posts if k == "done"]
    if not done or done[0][0] != 1:
        errs = [a[0] for k, a in posts if k == "log" and a[1] == "error"]
        return False, "下載失敗：" + (" | ".join(errs[-2:]) or "無訊息")
    return check_output(settings, vcodec, height)


def check_output(settings: av.Settings, vcodec: str | None, height: int | None) -> tuple[bool, str]:
    """檢查輸出資料夾：只有一個目標格式的檔案，串流、編碼、高度都對。

    Qt 版與 C# 版的測試（tests/smoke_native.py）也用這個函式，三個版本的標準一致。
    """
    want = settings.container if settings.mode == "video" else settings.audio_format
    files = [p for p in Path(settings.out_dir).iterdir() if p.is_file()]
    hits = [p for p in files if p.suffix.lower() == "." + want]
    if not hits:
        return False, f"找不到 .{want}，實際產出：{[p.name for p in files]}"

    extra = [p.name for p in files if p not in hits]
    if extra:
        return False, f"留下了中間檔：{extra}"

    info = probe(hits[0])
    kinds = sorted({s["codec_type"] for s in info.get("streams", [])})
    codecs = ",".join(s["codec_name"] for s in info.get("streams", []))
    need = ["audio", "video"] if settings.mode == "video" else ["audio"]
    if [k for k in need if k not in kinds]:
        return False, f"{hits[0].name} 缺少串流，只有 {kinds}"
    if settings.mode == "audio" and "video" in kinds and not settings.embed_thumb:
        return False, f"{hits[0].name} 音訊檔裡還有影像串流"
    video = [s for s in info.get("streams", [])
             if s["codec_type"] == "video" and not s.get("disposition", {}).get("attached_pic")]
    if vcodec and (not video or video[0]["codec_name"] != vcodec):
        return False, f"{hits[0].name} 影像應為 {vcodec}，實際 [{codecs}]"
    if height and (not video or video[0].get("height") != height):
        return False, f"{hits[0].name} 影像高度應為 {height}，實際 {video[0].get('height') if video else None}"
    return True, f"{hits[0].name}  [{codecs}]  {hits[0].stat().st_size / 1024:.0f} KB"


def main() -> int:
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("需要 ffmpeg 與 ffprobe")
        return 2
    work = Path(tempfile.mkdtemp(prefix="avd_smoke_"))
    srv, cases = make_fixtures(work)

    failed = 0
    for i, (name, s, src_url, vcodec, height) in enumerate(cases):
        s.out_dir = str(work / f"out_{i:02d}")
        Path(s.out_dir).mkdir()
        ok, detail = run_case(name, s, src_url, vcodec, height)
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  {name:<13} {detail}")

    srv.shutdown()
    shutil.rmtree(work, ignore_errors=True)
    print(f"\n{len(cases) - failed}/{len(cases)} 通過")
    return 1 if failed else 0


def make_fixtures(work: Path) -> tuple[http.server.ThreadingHTTPServer, list]:
    """產生測試素材、起本機 HTTP 服務，回傳（服務, 測試案例清單）。

    案例：(名稱, 設定, 網址, 期望影像編碼, 期望高度)；out_dir 由呼叫端填。
    """
    src = work / "src"
    src.mkdir()
    make_sample(src / "sample.mp4")
    make_dash(src / "sample.mp4", src / "dash")
    make_sample(src / "sample_av1.mp4", vcodec="libsvtav1")
    make_sample(src / "sample_vp9.webm", vcodec="libvpx-vp9")
    make_sample(src / "sample_240.mp4", size="426x240")
    make_mixed_dash(src / "sample_av1.mp4", src / "sample_240.mp4", src / "mixed")
    srv, base = serve(src)
    url = f"{base}/sample.mp4"
    dash = f"{base}/dash/manifest.mpd"
    av1 = f"{base}/sample_av1.mp4"
    webm = f"{base}/sample_vp9.webm"
    mixed = f"{base}/mixed/manifest.mpd"
    print(f"單一檔案來源：{url}")
    print(f"DASH 來源    ：{dash}（影像、聲音分開，需要合併）")
    print(f"AV1 單檔     ：{av1}")
    print(f"混合 DASH    ：{mixed}（AV1 360p + H.264 240p）\n")

    def video(c: str, priority: str = "quality") -> av.Settings:
        return av.Settings(mode="video", container=c, priority=priority, embed_meta=True)

    # (名稱, 設定, 網址, 期望影像編碼, 期望高度)
    cases = [(f"單檔→{c}", video(c), url, None, None) for c in av.VIDEO_CONTAINERS]
    cases += [(f"DASH→{c}", video(c), dash, None, None) for c in av.VIDEO_CONTAINERS]
    cases += [(f"音訊→{f}", av.Settings(mode="audio", audio_format=f, audio_bitrate="128", embed_meta=True),
               url, None, None) for f in av.AUDIO_FORMATS]
    # 取捨
    cases += [
        ("畫質 AV1→mp4", video("mp4"), av1, "av1", 360),              # 畫質優先不動編碼
        ("畫質 混合→mp4", video("mp4"), mixed, "av1", 360),           # 挑解析度高的 AV1
        ("相容 混合→mp4", video("mp4", "compat"), mixed, "h264", 240),  # 直接挑 H.264
        ("相容 H264→mp4", video("mp4", "compat"), url, "h264", 360),   # 已相容，原樣保留
        ("畫質 AV1→mov", video("mov"), av1, "h264", 360),             # mov 裝不下 AV1，必須重編碼
        ("相容 webm→mov", video("mov", "compat"), webm, "h264", 360),  # 單一 webm 來源
        ("相容 webm→mp4", video("mp4", "compat"), webm, "h264", 360),  # 先轉成 mkv 再改封裝 mp4
    ]
    cases += [(f"相容 AV1→{c}", video(c, "compat"), av1, "h264", 360)  # 只有 AV1，重新編碼
              for c in sorted(av.COMPAT_H264_CONTAINERS)]

    return srv, cases


if __name__ == "__main__":
    sys.exit(main())
