"""Qt 版與 C# 版的測試 —— 不開介面、不連任何外部網站。

兩部分：

1. 參數一致性：用各種設定組合，請兩個執行檔印出它們要交給 yt-dlp 的命令列參數
   （--headless args），再用 yt-dlp 自己的命令列解析器（yt_dlp.parse_options）把參數
   還原成 yt-dlp 內部選項，和 Python 版 build_ydl_options() 產生的選項逐項比對。
   比的是「yt-dlp 實際會怎麼做」—— 格式選擇、排序、合併容器、輸出檔名，以及後處理器
   的種類、參數與執行順序。三個版本在這裡一致，下載結果就一致。

2. 實際下載：用 py/tests/smoke_test.py 的同一批測試素材與案例，讓兩個執行檔走和視窗
   相同的下載流程（--headless download），用同一個 check_output() 檢查結果。

執行（Windows，先建置 qt 與 csharp）：
    py -3 tests\\smoke_native.py            兩個都測
    py -3 tests\\smoke_native.py qt         只測 Qt 版
    py -3 tests\\smoke_native.py --args     只做參數一致性（幾秒鐘）
"""
from __future__ import annotations

import itertools
import json
import shutil
import subprocess
import sys
import tempfile
from dataclasses import asdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
sys.path.insert(0, str(ROOT / "py" / "tests"))
import anyvideo_download as av  # noqa: E402
import smoke_test as st  # noqa: E402
import yt_dlp  # noqa: E402

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}

# Python 版有、命令列版刻意不同或不適用的選項，比對時略過
_IGNORED_PP_ARGS = {"when"}


def headless(exe: Path, *args: str, timeout: int = 300) -> tuple[int, list[str]]:
    r = subprocess.run([str(exe), "--headless", *args], capture_output=True, timeout=timeout)
    return r.returncode, r.stdout.decode("utf-8", "replace").splitlines()


def write_settings(s: av.Settings, path: Path) -> None:
    path.write_text(json.dumps(asdict(s), ensure_ascii=False), encoding="utf-8")


# ─── 1. 參數一致性 ──────────────────────────────────────────────────────────

def _normalize_pp(d: dict) -> dict:
    d = {k: v for k, v in d.items() if k not in _IGNORED_PP_ARGS}
    if d.get("key") == "EnsureH264":
        # Python 版傳清單，命令列傳逗號字串；沒給時外掛預設 h264
        v = d.get("video_ok", "h264")
        d["video_ok"] = ",".join(v) if isinstance(v, (list, tuple)) else v
    return d


def compare_with_python(s: av.Settings, cli_args: list[str]) -> list[str]:
    """回傳差異清單；空清單表示一致。"""
    py = av.build_ydl_options(s, av.find_ffmpeg())
    cli = yt_dlp.parse_options(cli_args).ydl_opts
    diffs = []

    def same(label, a, b):
        if a != b:
            diffs.append(f"{label}: Python={a!r} 命令列={b!r}")

    same("format", py["format"], cli.get("format"))
    same("format_sort", list(py.get("format_sort") or []), list(cli.get("format_sort") or []))
    same("merge_output_format", py.get("merge_output_format"), cli.get("merge_output_format"))
    same("outtmpl", py["outtmpl"], (cli.get("outtmpl") or {}).get("default"))
    same("noplaylist", py["noplaylist"], cli.get("noplaylist"))
    same("windowsfilenames", py["windowsfilenames"], cli.get("windowsfilenames"))
    same("ignoreerrors", py["ignoreerrors"], cli.get("ignoreerrors"))
    for k in ("retries", "fragment_retries", "extractor_retries", "socket_timeout",
              "concurrent_fragment_downloads"):
        same(k, py[k], cli.get(k))
    same("ffmpeg_location", py.get("ffmpeg_location"), cli.get("ffmpeg_location"))
    same("cookiesfrombrowser", (py.get("cookiesfrombrowser") or (None,))[0],
         (cli.get("cookiesfrombrowser") or (None,))[0])
    same("writesubtitles", bool(py.get("writesubtitles")), bool(cli.get("writesubtitles")))
    if py.get("writesubtitles"):
        same("subtitleslangs", py.get("subtitleslangs"), cli.get("subtitleslangs"))
    same("writethumbnail", bool(py.get("writethumbnail")), bool(cli.get("writethumbnail")))

    # 後處理器：種類與順序必須完全相同；Python 版給的每個參數，命令列版都要給同樣的值
    py_pps = [_normalize_pp(d) for d in py["postprocessors"]]
    cli_pps = [_normalize_pp(d) for d in cli.get("postprocessors", [])]
    same("postprocessors", [d["key"] for d in py_pps], [d["key"] for d in cli_pps])
    for p, c in zip(py_pps, cli_pps):
        for k, v in p.items():
            if c.get(k) != v:
                diffs.append(f"{p['key']}.{k}: Python={v!r} 命令列={c.get(k)!r}")
    return diffs


def settings_matrix() -> list[av.Settings]:
    out = []
    for c, prio, q, subs, thumb in itertools.product(
            av.VIDEO_CONTAINERS, ["quality", "compat"], ["最佳", "1080p"], [False, True], [False, True]):
        out.append(av.Settings(out_dir=r"C:\Temp\avd out", mode="video", container=c, priority=prio,
                               quality=q, embed_subs=subs, embed_thumb=thumb))
    for f, thumb in itertools.product(av.AUDIO_FORMATS, [False, True]):
        out.append(av.Settings(out_dir=r"C:\Temp\avd out", mode="audio", audio_format=f,
                               audio_bitrate="256", embed_thumb=thumb))
    # 原始品質（不轉檔）與音訊「原始格式」：三個版本的規則要一樣
    for q, subs, meta in itertools.product(["最佳", "1080p"], [False, True], [True, False]):
        out.append(av.Settings(out_dir=r"C:\Temp\avd out", mode="video", container="mkv", priority="original",
                               quality=q, embed_subs=subs, embed_meta=meta))
    out.append(av.Settings(out_dir=r"C:\Temp\avd out", mode="audio", audio_format=av.AUDIO_ORIGINAL_VALUE, embed_thumb=True))
    out.append(av.Settings(out_dir=r"C:\Temp\avd out", mode="audio", audio_format=av.AUDIO_ORIGINAL_VALUE, embed_meta=False))
    # 其他選項各來一組
    out.append(av.Settings(out_dir="C:/Temp/slash/", playlist=True, browser="Firefox", embed_meta=False))
    out.append(av.Settings(out_dir=r"D:\影片\下載", mode="audio", playlist=True, browser="Chrome"))
    return out


def test_args(names: list[str], work: Path) -> int:
    matrix = settings_matrix()
    failed = 0
    per_exe: dict[str, list[list[str]]] = {}
    for name in names:
        per_exe[name] = []
        bad = 0
        for i, s in enumerate(matrix):
            path = work / f"args_{i:03d}.json"
            write_settings(s, path)
            rc, lines = headless(EXES[name], "args", str(path), "https://example.com/watch?v=x")
            if rc != 0:
                print(f"FAIL  {name} args #{i}: 結束代碼 {rc}")
                bad += 1
                continue
            per_exe[name].append(lines)
            diffs = compare_with_python(s, lines)
            if diffs:
                bad += 1
                print(f"FAIL  {name} args #{i} {s.mode}/{s.container}/{s.priority}:")
                for d in diffs:
                    print("        " + d)
        print(f"{'PASS' if not bad else 'FAIL'}  {name:<7} 參數與 Python 版一致：{len(matrix) - bad}/{len(matrix)} 組設定")
        failed += bad

    if len(per_exe) == 2:
        # 兩個原生版的參數應該逐字相同（只有外掛目錄的位置不同）
        def strip(lines):
            return [("<plugins>" if j and lines[j - 1] == "--plugin-dirs" else a) for j, a in enumerate(lines)]
        a, b = (list(map(strip, v)) for v in per_exe.values())
        same = sum(x == y for x, y in zip(a, b))
        ok = same == len(a) == len(b)
        failed += not ok
        print(f"{'PASS' if ok else 'FAIL'}  Qt 與 C# 參數逐字相同：{same}/{len(a)} 組")
    return failed


# ─── 2. 實際下載 ────────────────────────────────────────────────────────────

def test_downloads(names: list[str], work: Path) -> int:
    srv, cases = st.make_fixtures(work)
    failed = 0
    try:
        for name in names:
            print(f"\n── {name} ──")
            bad = 0
            for i, (label, s, url, vcodec, height) in enumerate(cases):
                s.out_dir = str(work / f"{name}_out_{i:02d}")
                Path(s.out_dir).mkdir()
                path = work / f"{name}_case_{i:02d}.json"
                write_settings(s, path)
                rc, lines = headless(EXES[name], "download", str(path), url)
                done = [ln for ln in lines if ln.startswith("done\t")]
                if rc != 0 or done != ["done\t1\t0\t0"]:
                    errs = [ln.split("\t", 2)[-1] for ln in lines if ln.startswith("log\terror")]
                    ok, detail = False, f"下載失敗（rc={rc}）：" + (" | ".join(errs[-2:]) or "無訊息")
                else:
                    ok, detail = st.check_output(s, vcodec, height)
                bad += not ok
                print(f"{'PASS' if ok else 'FAIL'}  {label:<13} {detail}")
            print(f"{name}：{len(cases) - bad}/{len(cases)} 通過")
            failed += bad
    finally:
        srv.shutdown()
    return failed


def main() -> int:
    args = sys.argv[1:]
    only_args = "--args" in args
    names = [a for a in args if a in EXES] or list(EXES)
    missing = [n for n in names if not EXES[n].exists()]
    if missing:
        print("找不到執行檔，請先建置：" + "、".join(str(EXES[n]) for n in missing))
        return 2
    if not shutil.which("ffmpeg") or not shutil.which("ffprobe"):
        print("需要 ffmpeg 與 ffprobe")
        return 2

    work = Path(tempfile.mkdtemp(prefix="avd_native_"))
    try:
        failed = test_args(names, work)
        if not only_args:
            failed += test_downloads(names, work)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
