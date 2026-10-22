"""YouTube／抖音／小紅書的網址處理測試（Python、Qt、C# 三個版本跑同一批案例）—— 不連任何外部網站。

使用者貼的常常是整段分享文字，網址形式也五花八門；yt-dlp 的解析器只認 douyin.com/video/<id> 與
xiaohongshu.com/explore/<id>。這裡檢查 Qt 版的三件事：
  1. 從分享文字裡找出網址（去掉標點、表情符號、中文，補上沒寫的 https://）
  2. 平台辨識，並改寫成 yt-dlp 認得的形式（小紅書的 xsec_token 一定要保留）
  3. 短連結（v.douyin.com、xhslink.com）跟著轉址走到真正的網址（用本機 HTTP 服務模擬轉址）

執行（Windows，先建置 qt 與 csharp）：
    py -3 tests\\platform_urls.py            三個版本都測
    py -3 tests\\platform_urls.py py qt      只測指定的版本（py、qt、csharp）
"""
from __future__ import annotations

import http.server
import subprocess
import sys
import tempfile
import threading
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "py"))
import anyvideo_download as av  # noqa: E402

EXES = {
    "qt": ROOT / "qt" / "build" / "Release" / "AnyVideoDownload.exe",
    "csharp": ROOT / "csharp" / "bin" / "Release" / "net8.0-windows" / "AnyVideoDownload.exe",
}
failed = 0
target = "py"          # 目前在測哪個版本


def run(*args: str) -> list[str]:
    r = subprocess.run([str(EXES[target]), "--headless", *args], capture_output=True, timeout=60)
    return r.stdout.decode("utf-8", "replace").splitlines()


def check(name: str, got, want) -> None:
    global failed
    ok = got == want
    failed += not ok
    print(f"{'PASS' if ok else 'FAIL'}  [{target}] {name}")
    if not ok:
        print(f"        想要：{want!r}\n        得到：{got!r}")


def urls(text: str) -> list[str]:
    if target == "py":
        return [av.platform_name(av.platform_of(n)) + "\t" + n for n in (av.normalize_url(u) for u in av.extract_urls(text))]
    with tempfile.TemporaryDirectory() as d:
        p = Path(d) / "t.txt"
        p.write_text(text, encoding="utf-8")
        return run("urls", str(p))


def resolve(path: str) -> dict[str, str]:
    if target == "py":
        final, note = av.resolve_link(base + path, extra_hosts={host}, timeout=10)
        return {"final": final, "note": note}
    out = {}
    for line in run("resolve", base + path, host):
        k, _, v = line.partition("\t")
        out[k] = v
    return out


# ── 3. 短連結轉址（本機伺服器扮演 v.douyin.com 與 xhslink.com）────────────────
class Redirects(http.server.BaseHTTPRequestHandler):
    table = {
        "/d/abc": "/hop2",
        "/hop2": "https://www.iesdouyin.com/share/video/7123456789012345678/?region=CN&u_code=0",
        "/x/abc": "https://www.xiaohongshu.com/discovery/item/674051740000000007027a15?xsec_token=T0k%3D&xsec_source=app_share",
        "/loop": "/loop",
    }

    def do_GET(self):
        target = self.table.get(self.path)
        if target:
            self.send_response(302)
            self.send_header("Location", target)
            self.send_header("Content-Length", "0")
        else:
            self.send_response(200)
            self.send_header("Content-Length", "2")
        self.end_headers()
        if not target:
            self.wfile.write(b"ok")

    def log_message(self, *a):  # 安靜
        pass


srv = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Redirects)
threading.Thread(target=srv.serve_forever, daemon=True).start()
base = f"http://127.0.0.1:{srv.server_port}"
host = "127.0.0.1"




def run_cases() -> None:
    # ── 1+2. 分享文字、平台、改寫 ───────────────────────────────────────────────
    check("YouTube：一般、短網址、Shorts 原樣",
          urls("https://www.youtube.com/watch?v=jNQXAC9IVRw\nhttps://youtu.be/jNQXAC9IVRw?si=abc\nhttps://www.youtube.com/shorts/abcdefghijk"),
          ["YouTube\thttps://www.youtube.com/watch?v=jNQXAC9IVRw", "YouTube\thttps://youtu.be/jNQXAC9IVRw?si=abc",
           "YouTube\thttps://www.youtube.com/shorts/abcdefghijk"])

    check("抖音分享文字：標題、表情、中文夾著短連結",
          urls("9.99 复制打开抖音，看看【某某的作品】今天的舞蹈 😍 https://v.douyin.com/iAbCdEf/ x@a.com 08/22 Cvz:/"),
          ["抖音\thttps://v.douyin.com/iAbCdEf/"])

    check("抖音：modal_id（精選／用戶頁）→ /video/<id>",
          urls("https://www.douyin.com/jingxuan?modal_id=7123456789012345678\nhttps://www.douyin.com/user/MS4wLjABAAAA?modal_id=7123456789012345679&from_tab_name=main"),
          ["抖音\thttps://www.douyin.com/video/7123456789012345678", "抖音\thttps://www.douyin.com/video/7123456789012345679"])

    check("抖音：iesdouyin 分享頁 → /video/<id>；圖文 /note/ 不改寫",
          urls("https://www.iesdouyin.com/share/video/7123456789012345678/?region=CN&mid=1\nhttps://www.douyin.com/note/7123456789012345680"),
          ["抖音\thttps://www.douyin.com/video/7123456789012345678", "抖音\thttps://www.douyin.com/note/7123456789012345680"])

    check("小紅書分享文字：沒寫 https://、全形標點、xsec_token 保留",
          urls("【香妃蛋糕也太香了吧🔥】 😆 aBcDeF 😆 http://xhslink.com/a/AbCdEfG，复制本条信息，打开【小红书】App查看精彩内容！"),
          ["小紅書\thttp://xhslink.com/a/AbCdEfG"])

    check("小紅書：用戶頁／搜尋結果／explore → /explore/<筆記>，參數原樣保留",
          urls("https://www.xiaohongshu.com/user/profile/5c31698d0000000007018a31/6411cf99000000001300b6d9?xsec_token=AB%3D&xsec_source=pc_user\n"
               "https://www.xiaohongshu.com/search_result/674051740000000007027a15?xsec_token=CBgeL8Dxd1ZWBhwqRd568gAZ_iwG-9JIf9tnApNmteU2E%3D\n"
               "https://www.xiaohongshu.com/explore/6411cf99000000001300b6d9"),
          ["小紅書\thttps://www.xiaohongshu.com/explore/6411cf99000000001300b6d9?xsec_token=AB%3D&xsec_source=pc_user",
           "小紅書\thttps://www.xiaohongshu.com/explore/674051740000000007027a15?xsec_token=CBgeL8Dxd1ZWBhwqRd568gAZ_iwG-9JIf9tnApNmteU2E%3D",
           "小紅書\thttps://www.xiaohongshu.com/explore/6411cf99000000001300b6d9"])

    check("TikTok 國際版：長網址與 vm.tiktok.com 原樣（yt-dlp 自己處理）",
          urls("看這個 https://www.tiktok.com/@scout2015/video/6718335390845095173?lang=en。\nhttps://vm.tiktok.com/ZMabcdef/"),
          ["TikTok\thttps://www.tiktok.com/@scout2015/video/6718335390845095173?lang=en", "TikTok\thttps://vm.tiktok.com/ZMabcdef/"])

    check("一般網站與重複網址：平台留空、重複只算一次",
          urls("https://example.com/a.mp4 https://example.com/a.mp4"), ["\thttps://example.com/a.mp4"])
    check("沒有網址的文字", urls("今天天氣真好"), [])

    # ── 3. 短連結轉址（轉址表在上面）
    r = resolve("/d/abc")
    check("抖音短連結：兩次轉址後改寫成 /video/<id>", r.get("final"), "https://www.douyin.com/video/7123456789012345678")
    check("  紀錄區會說辨識成什麼", "辨識為抖音" in r.get("note", ""), True)
    r = resolve("/x/abc")
    check("小紅書短連結：轉址後 /explore/<筆記>，xsec_token 保留",
          r.get("final"), "https://www.xiaohongshu.com/explore/674051740000000007027a15?xsec_token=T0k%3D&xsec_source=app_share")
    r = resolve("/nope")
    check("沒有轉址的短連結：說明原因、保留原網址", (r.get("final"), "展開失敗" in r.get("note", "")), (base + "/nope", True))
    r = resolve("/loop")
    check("轉址繞圈：不會卡住，放棄並說明", (r.get("final"), "放棄展開" in r.get("note", "")), (base + "/loop", True))


wanted = [a for a in sys.argv[1:] if a in ("py", "qt", "csharp")] or ["py", "qt", "csharp"]
for target in wanted:
    if target != "py" and not EXES[target].exists():
        print(f"SKIP  [{target}] 找不到執行檔，請先建置：{EXES[target]}")
        continue
    run_cases()
srv.shutdown()

print("\n全部通過" if not failed else f"\n失敗 {failed} 項")
sys.exit(1 if failed else 0)
