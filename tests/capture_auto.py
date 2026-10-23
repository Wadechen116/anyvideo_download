"""「自動擷取」的端對端測試（Python 版）—— 不連任何外部網站，用 capture_flow.py 同一個本機網頁。

  1. 有影片的頁面：自動鎖定長度（4 秒）、下載、比對成功、瀏覽器被關掉；輸出是 hi.mp4（1280×2276、4 秒、有聲音）。
  2. 沒有影片的頁面（模擬要登入／驗證）：不下載、回報 "manual"，瀏覽器維持開著交給使用者。

執行（Windows）：  py -3 tests\\capture_auto.py
"""
from __future__ import annotations

import shutil
import sys
import tempfile
import threading
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "py" / "tests"))
sys.path.insert(0, str(ROOT / "tests"))
import avd_capture as ac  # noqa: E402
import capture_flow as cf  # noqa: E402
import smoke_test as st  # noqa: E402


def browser_alive(port: int) -> bool:
    try:
        urllib.request.urlopen(f"http://127.0.0.1:{port}/json/version", timeout=2).close()
        return True
    except OSError:
        return False


def run(url: str, profile: Path, out_dir: Path) -> tuple[str | None, str, ac.CaptureSession, list[str]]:
    lines: list[str] = []
    session = ac.CaptureSession(lambda c: None, lambda t: None, str(profile))
    session.start(url)
    import shutil as sh
    exe = sh.which("ffmpeg")
    path, reason = ac.auto_capture(session, str(out_dir), str(Path(exe).parent) if exe else None, lines.append, None, None, threading.Event())
    return path, reason, session, lines


def main() -> int:
    work = Path(tempfile.mkdtemp(prefix="avd_auto_"))
    failed = 0
    srv = None
    try:
        site = work / "site"
        site.mkdir()
        cf.make_site(site)
        (site / "empty.html").write_text("<!doctype html><meta charset=utf-8><title>login</title><p>請登入</p>", encoding="utf-8")
        srv, base = st.serve(site)
        srv.handle_error = lambda *a: None

        # 1. 有影片
        profile = work / "p1"
        path, reason, session, lines = run(base + "/index.html", profile, work / "out1")
        info = cf.probe(Path(path)) if path else {"streams": [], "format": {}}
        v = next((s for s in info["streams"] if s["codec_type"] == "video"), {})
        dur = float(info["format"].get("duration", 0) or 0)
        try:
            if reason == "ok":
                session.close_browser()      # 對話框成功時做的事
                import time
                time.sleep(2)
            closed = not browser_alive(session.port)
        finally:
            cf.kill_test_browser(profile)
        checks = {"原因是 ok": reason == "ok", "影像是 hi.mp4（1280×2276）": (v.get("width"), v.get("height")) == (1280, 2276),
                  "長度約 4 秒": 3.5 <= dur <= 4.6, "瀏覽器已關閉": closed}
        bad = [k for k, ok in checks.items() if not ok]
        failed += bool(bad)
        print(f"{'PASS' if not bad else 'FAIL'}  [auto/有影片] {reason}  {dur:.2f} 秒")
        for k in bad:
            print("        不符：" + k)
        if bad:
            print("\n".join("        " + l for l in lines[-12:]))

        # 2. 沒有影片 → 交給使用者
        ac.AUTO_WAIT_SECONDS = 8
        profile = work / "p2"
        path, reason, session, lines = run(base + "/empty.html", profile, work / "out2")
        try:
            alive = browser_alive(session.port)
            session.stop()
        finally:
            cf.kill_test_browser(profile)
        ok2 = reason == "manual" and path is None and alive
        failed += not ok2
        print(f"{'PASS' if ok2 else 'FAIL'}  [auto/需要手動] {reason}  瀏覽器維持開著={alive}")
    finally:
        if srv:
            srv.shutdown()
        shutil.rmtree(work, ignore_errors=True)
    print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
