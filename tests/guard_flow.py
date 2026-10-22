"""下載前的設定防呆測試（Python、Qt、C# 三個版本跑同一批案例）—— 不連任何外部網站。

防呆的規則（py/anyvideo_download.py、qt/src/guard.cpp、csharp/Guard.cs）在三個版本必須一模一樣。這裡用同一批
「設定 × 儲存位置狀況」案例，讓三個版本各自找出問題、各自套用修正，輸出逐項比對；另外對關鍵案例有明確的預期結果，
所以不是「三個都錯得一樣」也能抓到。

執行（Windows，先建置 qt 與 csharp）：
    py -3 tests\\guard_flow.py            三個版本都測
    py -3 tests\\guard_flow.py py qt      只測指定的版本（py、qt、csharp）
"""
from __future__ import annotations

import itertools
import json
import subprocess
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
import anyvideo_download as av  # noqa: E402

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}
GB = 1024 ** 3
GOOD_DIR = r"C:\Users\u\Videos"


def run_py(s: av.Settings, writable: bool, free: int) -> tuple[list, tuple]:
    issues = av.check_download_settings(s, writable=writable, free=None if free < 0 else free)
    fixed = av.apply_guard_fixes(s, issues)
    return ([(i.id, i.detail, int(i.fatal), ";".join(f"{k}={v}" for k, v in i.fix)) for i in issues],
            (fixed.container, int(fixed.embed_subs), int(fixed.embed_thumb), fixed.audio_format))


def run_exe(name: str, s: av.Settings, writable: bool, free: int, work: Path, n: int) -> tuple[list, tuple]:
    cfg = work / f"s{n}.json"
    cfg.write_text(json.dumps(asdict(s), ensure_ascii=False), encoding="utf-8")
    args = [str(EXES[name]), "--headless", "guard", str(cfg), f"writable={int(writable)}"] + ([f"free={free}"] if free >= 0 else [])
    out = subprocess.run(args, capture_output=True, timeout=60).stdout.decode("utf-8", "replace").splitlines()
    issues, fixed = [], ()
    for line in out:
        f = line.split("\t")
        if f[0] == "issue":
            issues.append((f[1], f[2], int(f[3]), f[4] if len(f) > 4 else ""))
        elif f[0] == "fixed":
            fixed = (f[1], int(f[2]), int(f[3]), f[4])
    return issues, fixed


def cases() -> list[tuple[str, av.Settings, bool, int]]:
    out = []
    base = dict(out_dir=GOOD_DIR)
    for c, prio, subs, thumb in itertools.product(av.VIDEO_CONTAINERS, ["quality", "compat", "original"], [False, True], [False, True]):
        out.append((f"video {c}/{prio} subs={subs} thumb={thumb}",
                    av.Settings(mode="video", container=c, priority=prio, embed_subs=subs, embed_thumb=thumb, **base), True, -1))
    for f, thumb in itertools.product(av.AUDIO_FORMATS + [av.AUDIO_ORIGINAL_VALUE], [False, True]):
        out.append((f"audio {f} thumb={thumb}", av.Settings(mode="audio", audio_format=f, embed_thumb=thumb, **base), True, -1))
    for d in [r"C:\Windows\Temp", "C:/Program Files/x", "C:/Program Files (x86)/a b", "D:\\", "D:/影片", r"C:\Users\u", "/mnt/c/windows/x",
              r"\\?\C:\Windows\x", "/etc/x", "/usr", "d:/models/out", r"C:\ProgramData\x", "E:/file.txt", "  C:\\Windows  "]:
        out.append((f"dir {d!r}", av.Settings(out_dir=d), True, -1))
    for label, writable, free in [("not writable", False, -1), ("1 GB free", True, GB), ("exactly 2 GB", True, 2 * GB), ("2 GB - 1", True, 2 * GB - 1),
                                  ("0.5 GB", True, GB // 2), ("lots", True, 500 * GB), ("unwritable and low", False, GB)]:
        out.append((f"env {label}", av.Settings(out_dir=GOOD_DIR), writable, free))
    out.append(("admin dir and not writable", av.Settings(out_dir=r"C:\Windows\x"), False, GB))
    return out


def main() -> int:
    wanted = [a for a in sys.argv[1:] if a in ("py", "qt", "csharp")] or ["py", "qt", "csharp"]
    missing = [str(EXES[n]) for n in wanted if n != "py" and not EXES[n].exists()]
    if missing:
        print("找不到執行檔，請先建置：" + "、".join(missing))
        return 2
    failed = 0
    allcases = cases()
    ref = {label: run_py(s, w, f) for label, s, w, f in allcases}

    def expect(label: str, ids: list[str], fixed: tuple | None = None, fatal: bool | None = None) -> None:
        nonlocal failed
        issues, fx = ref[label]
        ok = [i[0] for i in issues] == ids and (fixed is None or fx == fixed) and (fatal is None or any(i[2] for i in issues) == fatal)
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  預期結果：{label}" + ("" if ok else f"\n        想要 {ids} {fixed}\n        得到 {issues} {fx}"))

    # 關鍵案例的預期（以規則本身為準，不是「三個一致就好」）
    expect("video mp4/quality subs=False thumb=False", [])
    expect("video mp4/compat subs=True thumb=True", [])
    expect("video webm/compat subs=False thumb=False", ["compat_not_kept"], ("mp4", 0, 0, "mp3"))
    expect("video avi/compat subs=True thumb=True", ["compat_not_kept", "subs_dropped", "thumb_dropped"], ("mkv", 1, 1, "mp3"))
    expect("video avi/quality subs=True thumb=False", ["subs_dropped"], ("mkv", 1, 0, "mp3"))
    expect("video webm/quality subs=False thumb=True", ["thumb_dropped"], ("mkv", 0, 1, "mp3"))
    expect("video webm/original subs=True thumb=True", ["thumb_dropped"], ("webm", 1, 0, "mp3"))
    expect("video avi/original subs=True thumb=False", [])                    # 原始品質由來源決定容器，字幕照嵌
    expect("audio wav thumb=True", ["thumb_dropped"], ("mp4", 0, 1, "flac"))
    expect("audio best thumb=True", ["thumb_dropped"], ("mp4", 0, 0, "best"))
    expect("audio mp3 thumb=True", [])
    expect("dir 'C:\\\\Windows\\\\Temp'", ["dir_admin_only"], fatal=True)
    expect("dir 'D:\\\\'", ["dir_admin_only"], fatal=True)
    expect("dir 'D:/影片'", [])                                               # 根目錄底下自己建的資料夾不是管理員位置
    expect("dir 'C:\\\\Users\\\\u'", [])
    expect("dir '/mnt/c/windows/x'", ["dir_admin_only"], fatal=True)
    expect("dir 'E:/file.txt'", ["dir_admin_only"], fatal=True)               # 直接放在磁碟根目錄的檔案
    expect("dir '  C:\\\\Windows  '", ["dir_admin_only"], fatal=True)
    expect("env not writable", ["dir_not_writable"], fatal=True)
    expect("env 1 GB free", ["disk_low"], fatal=False)
    expect("env exactly 2 GB", [])
    expect("env 2 GB - 1", ["disk_low"])
    expect("env unwritable and low", ["dir_not_writable"])                   # 寫不進去就不必再說空間
    expect("admin dir and not writable", ["dir_admin_only"])                  # 管理員位置優先，不重複說
    # 修正之後不該再有問題（每個有修法的案例）
    for label, s, w, f in allcases:
        issues, _ = ref[label]
        if any(i[3] for i in issues):
            fixed = av.apply_guard_fixes(s, av.check_download_settings(s, writable=w, free=None if f < 0 else f))
            left = [i.id for i in av.check_download_settings(fixed, writable=w, free=None if f < 0 else f) if i.fix]
            if left:
                failed += 1
                print(f"FAIL  修正後仍有可修的問題：{label} -> {left}")
    print("PASS  每個有修法的案例，修正後沒有可修的問題" if not failed else "")

    # 三個版本逐項一致
    for name in wanted:
        if name == "py":
            continue
        with tempfile.TemporaryDirectory(prefix="avd_guard_") as d:
            bad = 0
            for n, (label, s, w, f) in enumerate(allcases):
                got = run_exe(name, s, w, f, Path(d), n)
                if got != ref[label]:
                    bad += 1
                    if bad <= 5:
                        print(f"FAIL  [{name}] {label}\n        Python：{ref[label]}\n        {name}：{got}")
            failed += bad
            print(f"{'PASS' if not bad else 'FAIL'}  [{name}] 與 Python 版一致：{len(allcases) - bad}/{len(allcases)} 個案例")
    print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
