"""「從瀏覽器擷取」的端對端測試（Python、Qt、C# 三個版本）—— 不連任何外部網站。

在本機起一個網頁：裡面有一個正在播放的 <video>（4 秒），另外用 fetch() 抓幾個檔案，模擬真實網站的行為：
  tv.mp4       <video> 正在播的那一檔（640×1138，只有影像）
  hi.mp4       同一支影片的較高畫質（1280×2276，4 秒）——播放器的自動畫質切換也會請求它
  au.mp4       同一支影片的聲音（4 秒、AAC 128k）
  decoy_v.mp4  「預先載入的下一支影片」：影像 1440×2560、6 秒，解析度比 hi 還高
  decoy_a.mp4  那一支的聲音：6 秒、AAC 256k，位元率比 au 還高
用真的 Chrome／Edge 開這個網頁，讓程式擷取並下載。期望的結果：
  影像取 hi.mp4（正在播放的那支裡最高畫質的），聲音取 au.mp4，兩個「下一支」的檔案（解析度與位元率都更高）要被長度篩掉；
  輸出是一個 mp4，影像 1280×2276、有 AAC 聲音、長度約 4 秒。

用暫存的瀏覽器設定檔（環境變數 AVD_CAPTURE_PROFILE），不碰你平常的擷取設定檔。會短暫開出 Chrome／Edge 視窗。

執行（Windows，先建置 qt 與 csharp）：
    py -3 tests\\capture_flow.py            三個版本都測
    py -3 tests\\capture_flow.py py qt      只測指定的版本（py、qt、csharp）
"""
from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "py" / "tests"))
import smoke_test as st  # noqa: E402  (只用它的 serve())

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}


def ffmpeg(*args: str) -> None:
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", *args], check=True)


def make_site(root: Path) -> None:
    def video(name: str, size: str, dur: int) -> None:
        ffmpeg("-f", "lavfi", "-i", f"testsrc2=size={size}:rate=25:duration={dur}", "-c:v", "libx264", "-preset", "ultrafast",
               "-pix_fmt", "yuv420p", "-an", str(root / name))

    def audio(name: str, dur: int, kbps: int, hz: int) -> None:
        ffmpeg("-f", "lavfi", "-i", f"sine=frequency={hz}:duration={dur}", "-c:a", "aac", "-b:a", f"{kbps}k", str(root / name))

    video("tv.mp4", "640x1138", 4)
    video("hi.mp4", "1280x2276", 4)
    audio("au.mp4", 4, 128, 440)
    video("decoy_v.mp4", "1440x2560", 6)
    audio("decoy_a.mp4", 6, 256, 880)
    (root / "index.html").write_text(
        "<!doctype html><meta charset=utf-8><title>capture test</title>"
        "<video id=v src='/tv.mp4' muted autoplay loop playsinline width=320></video>"
        "<script>['hi.mp4','au.mp4','decoy_v.mp4','decoy_a.mp4'].forEach((f,i)=>"
        "setTimeout(()=>fetch('/'+f).then(r=>r.arrayBuffer()),400*(i+1)));</script>", encoding="utf-8")


def probe(path: Path) -> dict:
    r = subprocess.run(["ffprobe", "-v", "error", "-print_format", "json", "-show_streams", "-show_format", str(path)],
                       capture_output=True, text=True, encoding="utf-8", errors="replace")
    return json.loads(r.stdout)


def kill_test_browser(profile: Path) -> None:
    """測試開的瀏覽器（只認這次的暫存設定檔），用完要關，否則設定檔資料夾刪不掉。"""
    ps = ("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*" + str(profile).replace("'", "''") +
          "*' } | ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }")
    subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, timeout=60)


def capture(target: str, url: str, out_dir: Path, profile: Path) -> tuple[bool, Path | None, str]:
    """用指定版本擷取，回傳（成功, 輸出檔, 紀錄）。"""
    env = {**os.environ, "AVD_CAPTURE_PROFILE": str(profile), "AVD_WAIT_SECS": "4"}
    out_dir.mkdir(parents=True, exist_ok=True)
    if target == "py":
        import avd_capture
        lines: list[str] = []
        path = avd_capture.run_headless(url, str(out_dir), wait_seconds=4, profile=str(profile), log=lines.append)
        return path is not None, Path(path) if path else None, "\n".join(lines)
    if target == "qt":
        png = out_dir / "dialog.png"
        subprocess.run([str(EXES["qt"]), "--selftest-live", url, str(png), str(out_dir)], env=env, timeout=240)
        log = (out_dir / "dialog.png.txt").read_text(encoding="utf-8") if (out_dir / "dialog.png.txt").exists() else ""
    else:
        r = subprocess.run([str(EXES["csharp"]), "--headless", "capture", url, str(out_dir), "4"], env=env, capture_output=True, timeout=240)
        log = r.stdout.decode("utf-8", "replace")
    m = re.findall(r"完成：(.+)", log)
    return bool(m), Path(m[-1].strip()) if m else None, log


def main() -> int:
    wanted = [a for a in sys.argv[1:] if a in ("py", "qt", "csharp")] or ["py", "qt", "csharp"]
    missing = [str(EXES[n]) for n in wanted if n != "py" and not EXES[n].exists()]
    if missing:
        print("找不到執行檔，請先建置：" + "、".join(missing))
        return 2
    work = Path(tempfile.mkdtemp(prefix="avd_capture_"))
    failed = 0
    srv = None
    try:
        site = work / "site"
        site.mkdir()
        make_site(site)
        srv, base = st.serve(site)
        srv.handle_error = lambda *a: None          # Chrome 播放時會主動中斷連線（ConnectionResetError），不是錯誤
        for target in wanted:
            profile = work / f"profile_{target}"
            out_dir = work / f"out_{target}"
            try:
                ok, out, log = capture(target, base + "/index.html", out_dir, profile)
            finally:
                kill_test_browser(profile)
            if not ok or out is None or not out.exists():
                failed += 1
                print(f"FAIL  [{target}] 沒有產出檔案\n" + "\n".join("        " + l for l in log.splitlines()[-12:]))
                continue
            info = probe(out)
            v = next((s for s in info["streams"] if s["codec_type"] == "video"), {})
            a = next((s for s in info["streams"] if s["codec_type"] == "audio"), {})
            dur = float(info["format"].get("duration", 0))
            checks = {
                "影像是 hi.mp4（1280×2276）": (v.get("width"), v.get("height")) == (1280, 2276),
                "有聲音（AAC）": a.get("codec_name") == "aac",
                "長度約 4 秒（不是預載的 6 秒）": 3.5 <= dur <= 4.6,
                "聲音是 au.mp4（128k，不是 256k 的）": 60_000 <= int(a.get("bit_rate", 0) or 0) <= 200_000,
                "只有一個輸出檔": len([p for p in out_dir.iterdir() if p.suffix == ".mp4"]) == 1,
            }
            bad = [k for k, ok in checks.items() if not ok]
            failed += bool(bad)
            print(f"{'PASS' if not bad else 'FAIL'}  [{target}] {out.name}  影像 {v.get('width')}×{v.get('height')}  聲音 {a.get('codec_name')} {a.get('bit_rate')}  {dur:.2f} 秒")
            for k in bad:
                print("        不符：" + k)
            if bad:
                print("\n".join("        " + l for l in log.splitlines()[-14:]))
    finally:
        if srv:
            srv.shutdown()
        shutil.rmtree(work, ignore_errors=True)
    print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
