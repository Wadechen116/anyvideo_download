"""「原始品質（不轉檔）」的測試（Python、Qt、C# 三個版本）—— 不連任何外部網站。

「原始品質」的承諾是：下載到的影像與聲音，和網站提供的串流逐格相同，沒有任何損耗。這裡用 ffmpeg 產生來源
（H.264、AV1、VP9、影像聲音分開的 DASH、AV1 與 H.264 混合的 DASH），在本機起 HTTP 服務，讓 Qt 版用
priority=original 下載，再把「來源」與「結果」都解碼，比對每一格畫面與每個聲音取樣的雜湊值：相同才算沒有損耗。
（只比封包不行：同一個串流放進 mp4 或 mkv，封包的表示法會不同，但解碼出來的畫面一樣。）

執行（Windows，先建置 qt 與 csharp）：
    py -3 tests\\original_quality.py            三個版本都測
    py -3 tests\\original_quality.py py qt      只測指定的版本（py、qt、csharp）
"""
from __future__ import annotations

import json
import shutil
import subprocess
import sys
import tempfile
import threading
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "py" / "tests"))
import anyvideo_download as av  # noqa: E402
import smoke_test as st  # noqa: E402

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}


def decoded_md5(path: Path, kind: str) -> str:
    """解碼後的畫面（kind='v'）或聲音（'a'）的 MD5：播放器實際會看到、聽到的內容。"""
    sel = ["-map", "0:v:0"] if kind == "v" else ["-map", "0:a:0", "-ar", "48000", "-ac", "2"]
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), *sel, "-f", "md5", "-"], capture_output=True, text=True)
    return r.stdout.strip() or "(" + r.stderr.strip()[-80:] + ")"


def packet_md5(path: Path, kind: str) -> str:
    """串流本身（封包的位元組）的 MD5，不解碼。換容器（webm→ogg、mp4→mkv）時串流原封不動，這個值就不變。"""
    r = subprocess.run(["ffmpeg", "-v", "error", "-i", str(path), "-map", "0:" + kind + ":0", "-c", "copy", "-f", "md5", "-"], capture_output=True, text=True)
    return r.stdout.strip() or "(" + r.stderr.strip()[-80:] + ")"


def same_sound(a: Path, b: Path) -> bool:
    """聲音相同：解碼後相同，或串流位元組相同。後者是為了 Opus：同一個串流放在 webm 與 ogg，起點修剪的寫法不同，
    解碼出來的取樣數會差幾個，但串流本身一個位元組都沒變。"""
    return decoded_md5(a, "a") == decoded_md5(b, "a") or packet_md5(a, "a") == packet_md5(b, "a")


def streams(path: Path) -> list[str]:
    r = subprocess.run(["ffprobe", "-v", "error", "-show_entries", "stream=codec_type,codec_name,height", "-of", "json", str(path)],
                       capture_output=True, text=True)
    return [f"{s['codec_type']}:{s['codec_name']}" + (f":{s['height']}" if "height" in s else "") for s in json.loads(r.stdout)["streams"]]


def download(s: av.Settings, url: str, work: Path, name: str) -> tuple[bool, Path | None, str]:
    s.out_dir = str(work / name)
    Path(s.out_dir).mkdir()
    if target == "py":      # 函式庫版：直接在這個執行緒跑下載工作
        posts: list = []
        av.DownloadJob([url], s, av.find_ffmpeg(), lambda kind, *a: posts.append((kind, a)), threading.Event()).run()
        done = [a for k, a in posts if k == "done"]
        ok = bool(done) and done[0][0] == 1
        err = " | ".join(a[0] for k, a in posts if k == "log" and a[1] == "error")[-200:]
    else:
        cfg = work / f"{name}.json"
        cfg.write_text(json.dumps(asdict(s), ensure_ascii=False), encoding="utf-8")
        r = subprocess.run([str(EXES[target]), "--headless", "download", str(cfg), url], capture_output=True, timeout=300)
        lines = r.stdout.decode("utf-8", "replace").splitlines()
        ok = r.returncode == 0 and "done\t1\t0\t0" in lines
        err = " | ".join(ln.split("\t", 2)[-1] for ln in lines if ln.startswith("log\terror"))[-200:]
    files = [p for p in Path(s.out_dir).iterdir() if p.is_file()]
    return ok and len(files) == 1, files[0] if files else None, err


target = "py"          # 目前在測哪個版本


def main() -> int:
    global target
    wanted = [a for a in sys.argv[1:] if a in ("py", "qt", "csharp")] or ["py", "qt", "csharp"]
    missing = [str(EXES[n]) for n in wanted if n != "py" and not EXES[n].exists()]
    if missing:
        print("找不到執行檔，請先建置：" + "、".join(missing))
        return 2
    work = Path(tempfile.mkdtemp(prefix="avd_orig_"))
    failed = 0
    try:
        src = work / "src"
        src.mkdir()
        st.make_sample(src / "h264.mp4")
        st.make_sample(src / "av1.mp4", vcodec="libsvtav1")
        st.make_sample(src / "vp9.webm", vcodec="libvpx-vp9")
        st.make_sample(src / "h264_240.mp4", size="426x240")
        st.make_dash(src / "av1.mp4", src / "dash")
        st.make_mixed_dash(src / "av1.mp4", src / "h264_240.mp4", src / "mixed")
        srv, base = st.serve(src)
        try:
            # (名稱, 網址, 來源檔（比對解碼結果用）, 期望的影像串流)
            video_cases = [
                ("H.264 單檔", f"{base}/h264.mp4", src / "h264.mp4", "video:h264"),
                ("AV1 單檔", f"{base}/av1.mp4", src / "av1.mp4", "video:av1"),
                ("VP9 單檔", f"{base}/vp9.webm", src / "vp9.webm", "video:vp9"),
                ("AV1 DASH（影像聲音分開）", f"{base}/dash/manifest.mpd", src / "av1.mp4", "video:av1"),
                ("混合 DASH：挑最高的 AV1 360p", f"{base}/mixed/manifest.mpd", src / "av1.mp4", "video:av1:360"),
            ]
            for target in wanted:
                for name, url, source, expect in video_cases:
                    s = av.Settings(mode="video", container="mp4", priority="original", embed_meta=False)
                    ok, out, err = download(s, url, work, target + "_v" + str(abs(hash(name)) % 10**6))
                    if not ok:
                        print(f"FAIL  [{target}] {name:<24} 下載失敗：{err or '無訊息'}")
                        failed += 1
                        continue
                    st_out = streams(out)
                    pic = decoded_md5(out, "v") == decoded_md5(source, "v")
                    snd = same_sound(out, source)
                    has_expect = any(x.startswith(expect) for x in st_out)
                    good = pic and snd and has_expect
                    failed += not good
                    print(f"{'PASS' if good else 'FAIL'}  [{target}] {name:<24} {out.suffix} {st_out}  畫面相同={pic} 聲音相同={snd}")

                # 僅音訊「原始（不轉檔）」：聲音原封不動，副檔名跟著原本的編碼
                for name, url, source in [("音訊原始 AAC", f"{base}/h264.mp4", src / "h264.mp4"), ("音訊原始 Opus", f"{base}/vp9.webm", src / "vp9.webm")]:
                    s = av.Settings(mode="audio", audio_format="best", embed_meta=False)
                    ok, out, err = download(s, url, work, target + "_a" + str(abs(hash(name)) % 10**6))
                    if not ok:
                        print(f"FAIL  [{target}] {name:<24} 下載失敗：{err or '無訊息'}")
                        failed += 1
                        continue
                    same = same_sound(out, source)
                    failed += not same
                    print(f"{'PASS' if same else 'FAIL'}  [{target}] {name:<24} {out.suffix} {streams(out)}  聲音相同={same}")

                # 對照：mp3 一定是重新編碼（有損），測試要能分辨出來
                s = av.Settings(mode="audio", audio_format="mp3", audio_bitrate="192", embed_meta=False)
                ok, out, err = download(s, f"{base}/h264.mp4", work, target + "_mp3")
                lossy = ok and decoded_md5(out, "a") != decoded_md5(src / "h264.mp4", "a")
                failed += not lossy
                print(f"{'PASS' if lossy else 'FAIL'}  [{target}] 對照：mp3 是有損的（解碼後與來源不同）")
        finally:
            srv.shutdown()
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
