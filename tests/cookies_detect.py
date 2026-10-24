"""cookies.txt 自動偵測測試（Python、Qt、C# 三個版本跑同一批檔案）—— 不連網路。

在暫存資料夾做幾個檔案、明確設定修改時間，檢查 detect_cookie_files 找到哪些、怎麼排序：
含抖音/TikTok → 檔名含 cookie → 較新 → 路徑。Qt／C# 版透過
    AnyVideoDownload.exe --headless cookies-detect <資料夾>...
每個找到的檔案印一行 "found<TAB>絕對路徑"，最好的排最前面。

執行（Windows）：
    py -3 tests\\cookies_detect.py            三個版本都測
    py -3 tests\\cookies_detect.py py qt      只測指定的版本（py、qt、csharp）
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
import anyvideo_download as av  # noqa: E402

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}
failed = 0
skipped = 0
HDR = "# Netscape HTTP Cookie File\n"
LINE = "{d}\tTRUE\t/\tTRUE\t2000000000\tsid\tabc\n"


def check(target: str, name: str, got, want) -> None:
    global failed
    ok = got == want
    failed += not ok
    print(f"{'PASS' if ok else 'FAIL'}  [{target}] {name}")
    if not ok:
        print(f"        想要：{want!r}\n        得到：{got!r}")


def norm(p: str) -> str:
    return os.path.normcase(os.path.abspath(p))


def detect(target: str, dirs: list[str]):
    """回傳路徑清單；執行檔不支援時回傳 None。"""
    if target == "py":
        return av.detect_cookie_files(dirs)
    r = subprocess.run([str(EXES[target]), "--headless", "cookies-detect", *dirs], capture_output=True, timeout=60)
    out = r.stdout.decode("utf-8", "replace").splitlines()
    got = [l.split("\t", 1)[1] for l in out if l.startswith("found\t")]
    if not got and r.returncode != 0:
        return None
    return got


def main() -> None:
    global skipped
    with tempfile.TemporaryDirectory() as tmp:
        a = Path(tmp) / "a"
        b = Path(tmp) / "b"
        (a / "sub").mkdir(parents=True)
        b.mkdir()
        now = time.time()
        files = {
            a / "my_cookies.txt": (HDR + LINE.format(d=".douyin.com") + LINE.format(d=".example.com"), now - 5000),
            a / "other.txt": (HDR + LINE.format(d=".example.com"), now - 10),          # 較新、沒有抖音
            a / "notes.txt": ("hello\nthis is not a cookie file\n", now - 1),
            a / "sub" / "deep_cookies.txt": (HDR + LINE.format(d=".douyin.com"), now),   # 子資料夾，要忽略
            a / "empty.txt": ("", now),
            b / "export.txt": ("﻿" + LINE.format(d="#HttpOnly_.tiktok.com").replace("\n", "\r\n")
                               + LINE.format(d=".tiktok.com").replace("\n", "\r\n"), now - 100),   # BOM、CRLF、無標頭
            b / "big.txt": (HDR + "x" * (5 * 1024 * 1024 + 10), now),                 # 超過 5 MB
            b / "readme.md": (HDR + LINE.format(d=".douyin.com"), now),                # 不是 .txt
            b / "plain_new.txt": (LINE.format(d=".example.com"), now - 50),            # 無標頭但有 cookie 行
        }
        for p, (text, mt) in files.items():
            p.write_bytes(text.encode("utf-8"))
            os.utime(p, (mt, mt))
        # 排序：含抖音且檔名有 cookie → 含 tiktok（export.txt，檔名無 cookie）
        # → 無抖音：檔名無 cookie，較新者先（other.txt 10 秒前 > plain_new.txt 50 秒前）
        want = [norm(str(x)) for x in (a / "my_cookies.txt", b / "export.txt", a / "other.txt", b / "plain_new.txt")]
        dirs = [str(a), str(b), str(Path(tmp) / "missing"), str(a)]   # 含不存在的資料夾與重複

        for target in ("py", "qt", "csharp"):
            if target not in wanted:
                continue
            if target != "py" and not EXES[target].exists():
                print(f"SKIP  [{target}] 找不到執行檔，請先建置：{EXES[target]}")
                skipped += 1
                continue
            got = detect(target, dirs)
            if got is None:
                print(f"UNSUPPORTED  [{target}] 執行檔還不支援 --headless cookies-detect（沒有輸出 found 行）")
                skipped += 1
                continue
            check(target, "有效檔案與排序（抖音 → 檔名 cookie → 新 → 路徑；忽略子資料夾／過大／空檔／非 txt）",
                  [norm(p) for p in got], want)
            check(target, "資料夾都沒有 cookies → 空清單", detect(target, [str(Path(tmp) / "missing")]) or [], [])
            # 同樣有抖音、檔名都有 cookie、時間相同 → 路徑字母順序
            c = Path(tmp) / "c"
            c.mkdir(exist_ok=True)
            for n in ("b_cookies.txt", "a_cookies.txt"):
                (c / n).write_bytes((HDR + LINE.format(d=".douyin.com")).encode())
                os.utime(c / n, (now - 7, now - 7))
            got = detect(target, [str(c)]) or []
            check(target, "全部同分時，路徑字母順序", [norm(p) for p in got],
                  [norm(str(c / "a_cookies.txt")), norm(str(c / "b_cookies.txt"))])


wanted = [a for a in sys.argv[1:] if a in ("py", "qt", "csharp")] or ["py", "qt", "csharp"]
main()
print(f"\n（{skipped} 個版本略過／不支援）" if skipped else "")
print("全部通過" if not failed else f"失敗 {failed} 項")
sys.exit(1 if failed else 0)
