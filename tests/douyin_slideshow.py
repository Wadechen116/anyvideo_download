"""DouyinSlideshow 後處理器的測試：用 ffmpeg 造假的圖片與音樂，攔截網路請求，檢查合成出的 mp4。
不需要網路也不需要 cookies。aweme detail 的結構（images[].url_list）依 yt-dlp 抖音解析器所用的欄位。"""
import json
import subprocess
import sys
import tempfile
from pathlib import Path

import yt_dlp

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
import anyvideo_download as av  # noqa: E402

PP = av._DouyinSlideshowPP
assert PP, "找不到 douyin_slideshow.py"
ffmpeg = av.find_ffmpeg()
exe = str(Path(ffmpeg) / "ffmpeg") if ffmpeg else "ffmpeg"
probe = str(Path(ffmpeg) / "ffprobe") if ffmpeg else "ffprobe"
tmp = Path(tempfile.mkdtemp(prefix="avd_slide_"))


def run(*a):
    subprocess.run([exe, "-y", "-loglevel", "error", *a], check=True)


imgs = {}
for i, (col, size, ext) in enumerate([("red", "720x1280", "jpg"), ("green", "1080x1080", "png"), ("blue", "540x960", "webp")]):
    f = tmp / f"src{i}.{ext}"
    run("-f", "lavfi", "-i", f"color=c={col}:s={size}", "-frames:v", "1", str(f))
    imgs[f"https://img.test/{i}.{ext}"] = f.read_bytes()
audio = tmp / "title [123].mp3"
run("-f", "lavfi", "-i", "sine=frequency=440:duration=9", "-b:a", "128k", str(audio))
detail = {"aweme_detail": {"images": [{"url_list": [u.replace(".webp", ".heic"), u]} for u in imgs]}}
imgs.update({u.replace(".webp", ".heic"): b"" for u in list(imgs) if u.endswith(".webp")})


class FakeResp:
    def __init__(self, b): self.b = b
    def read(self): return self.b


class FakeYdl(yt_dlp.YoutubeDL):
    def urlopen(self, req):
        u = req.url
        return FakeResp(json.dumps(detail).encode() if "aweme/detail" in u else imgs[u])


def make(info):
    ydl = FakeYdl({"ffmpeg_location": ffmpeg, "quiet": True})
    return PP(ydl).run(info)


fails = 0
def check(name, ok):
    global fails
    print(("PASS " if ok else "FAIL ") + name)
    fails += 0 if ok else 1


# 圖文：音樂換成 mp4，長度 ~ 音樂長度，畫布 = 第一張圖的尺寸（720x1280），H.264 + AAC
info = {"extractor_key": "Douyin", "id": "123", "filepath": str(audio), "ext": "mp3"}
dele, out = make(info)
mp4 = Path(out["filepath"])
check("輸出 mp4 存在", mp4.exists() and out["ext"] == "mp4")
check("舊聲音檔交給 yt-dlp 刪除", dele == [str(audio)])
r = json.loads(subprocess.run([probe, "-v", "error", "-show_entries", "stream=codec_name,width,height:format=duration",
                               "-of", "json", str(mp4)], capture_output=True, text=True).stdout)
codecs = [s["codec_name"] for s in r["streams"]]
check(f"H.264 + AAC {codecs}", codecs == ["h264", "aac"])
check(f"畫布 = 第一張圖 720x1280 ({r['streams'][0]['width']}x{r['streams'][0]['height']})", (r["streams"][0]["width"], r["streams"][0]["height"]) == (720, 1280))
check(f"長度~9 秒 ({r['format']['duration']})", abs(float(r["format"]["duration"]) - 9) < 0.7)

# 選網址：可解的格式優先，其中尺寸上限最大的；noop（原檔）最大；heic／avif 最後；沒有尺寸資訊的最小
import importlib.util
spec = importlib.util.spec_from_file_location("ds", ROOT / "shared" / "yt-dlp-plugins" / "anyvideo" / "yt_dlp_plugins" / "postprocessor" / "douyin_slideshow.py")
ds = importlib.util.module_from_spec(spec); spec.loader.exec_module(ds)
base = "https://p3.douyinpic.com/tos-cn-i-dy/abc"
pick = lambda *u: ds._pick_url({"url_list": [base + x for x in u]})
check("取尺寸上限最大的", pick("~tplv-dy-aweme-images:q75.webp", "~tplv-dy-aweme-images-v2:1080:1080:q75.webp", "~tplv-dy-aweme-images-v2:3000:3000:q75.webp") .endswith("3000:3000:q75.webp"))
check("heic 排在可解的格式之後", pick("~noop.heic", "~tplv-dy-aweme-images:q75.jpeg").endswith(".jpeg"))
check("noop（原檔）勝過有尺寸的", pick("~tplv-dy-aweme-images-v2:3000:3000:q75.webp", "~noop.jpeg").endswith("~noop.jpeg"))
check("只有 heic 時仍回傳它", pick("~noop.heic").endswith(".heic"))
check("同分取最前面", pick("~a.jpeg", "~b.jpeg").endswith("~a.jpeg"))
check("畫布：高解析不縮小、奇數取偶數", ds.canvas_size(1086, 1448) == (1086, 1448) and ds.canvas_size(1087, 1449) == (1086, 1448))
check("畫布：最長邊上限 4096", ds.canvas_size(6000, 3000) == (4096, 2048))

# 不是圖文／不是抖音：什麼都不做
detail["aweme_detail"] = {"video": {}}
info2 = {"extractor_key": "Douyin", "id": "1", "filepath": str(audio), "ext": "mp3"}
check("沒有 images → 不動", make(info2) == ([], info2))
info3 = {"extractor_key": "Youtube", "id": "1", "filepath": str(audio), "ext": "mp3"}
check("非抖音 → 不動", make(info3) == ([], info3))
info4 = {"extractor_key": "Douyin", "id": "1", "filepath": str(mp4), "ext": "mp4"}
check("已是影片 → 不動", make(info4) == ([], info4))

# 選項：影片模式帶 DouyinSlideshow 且排在 EnsureH264 前面，音訊模式不帶
for prio, cont in [("quality", "mp4"), ("compat", "mp4"), ("original", "mkv"), ("compat", "mov")]:
    keys = [d["key"] for d in av.build_ydl_options(av.Settings(out_dir=str(tmp), container=cont, priority=prio), ffmpeg)["postprocessors"]]
    ok = keys[0] == "DouyinSlideshow" and keys.count("DouyinSlideshow") == 1
    check(f"{prio}/{cont} PP 順序 {keys[:3]}", ok)
keys = [d["key"] for d in av.build_ydl_options(av.Settings(out_dir=str(tmp), mode="audio"), ffmpeg)["postprocessors"]]
check("音訊模式不含 DouyinSlideshow", "DouyinSlideshow" not in keys)
sys.exit(1 if fails else 0)
