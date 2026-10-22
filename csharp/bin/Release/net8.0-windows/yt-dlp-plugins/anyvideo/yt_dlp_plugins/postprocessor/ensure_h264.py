"""yt-dlp plugin: EnsureH264 —— Qt 版與 C# 版共用的編碼把關。

Python 版把同樣的邏輯寫在 py/anyvideo_download.py 的 EnsureH264PP 裡；Qt 版與
C# 版呼叫 yt-dlp 命令列，沒辦法把自訂後處理器插進去，所以包成 yt-dlp 外掛，
用下面的參數啟用：

    --plugin-dirs <程式目錄>/yt-dlp-plugins
    --use-postprocessor "EnsureH264:video_ok=h264"          相容優先
    --use-postprocessor "EnsureH264:video_ok=h264,hevc"     畫質優先 + mov

--use-postprocessor 在 yt-dlp 命令列永遠排在所有內建後處理器前面，也就是
「重新封裝／轉檔 → 字幕 → 資訊 → 封面」之前，正好是這一道需要的位置：
重新編碼只保留第一條影像與聲音串流，排在後面會把嵌進去的東西洗掉。

做的事：影像不在 video_ok 裡、或聲音不是 AAC／MP3，就重新編碼成 H.264／AAC。
已經相容的那一邊直接複製；兩邊都相容就什麼都不做。
"""
import os

from yt_dlp.postprocessor.ffmpeg import FFmpegPostProcessor, FFmpegPostProcessorError
from yt_dlp.utils import PostProcessingError

# 裝得下 H.264 的容器；其他的（例如網站只給一個 .webm）重新編碼時改存 mkv
_H264_CONTAINERS = {"mp4", "m4v", "mkv", "mov"}
_AUDIO_OK = {"aac", "mp3"}


class EnsureH264PP(FFmpegPostProcessor):
    def __init__(self, downloader=None, video_ok="h264", **kwargs):
        super().__init__(downloader)
        # 命令列傳進來的參數都是字串，例如 "h264,hevc"
        if isinstance(video_ok, str):
            video_ok = video_ok.split(",")
        self._video_ok = {v.strip() for v in video_ok if v.strip()}

    def _codecs(self, path):
        streams = self.get_metadata_object(path).get("streams", [])
        vcodec = acodec = None
        for st in streams:
            # 嵌入的封面圖也是一條影像串流，要略過
            if st.get("codec_type") == "video" and not (st.get("disposition") or {}).get("attached_pic"):
                vcodec = vcodec or st.get("codec_name")
            elif st.get("codec_type") == "audio":
                acodec = acodec or st.get("codec_name")
        return vcodec, acodec

    @FFmpegPostProcessor._restrict_to(images=False)
    def run(self, info):
        path = info.get("filepath")
        if not path or not os.path.exists(path):
            return [], info

        vcodec, acodec = self._codecs(path)
        v_ok = vcodec is None or vcodec in self._video_ok
        a_ok = acodec is None or acodec in _AUDIO_OK
        if v_ok and a_ok:
            self.to_screen(f"已是相容編碼（{vcodec}/{acodec}），不需要重新編碼")
            return [], info

        self.to_screen(f"網站沒有提供相容編碼，重新編碼 {vcodec}/{acodec} → h264/aac，會比較慢")
        root, ext = os.path.splitext(path)
        ext = ext[1:].lower()
        if ext not in _H264_CONTAINERS:
            ext = "mkv"
        dst = f"{root}.{ext}"
        tmp = f"{root}.compat-tmp.{ext}"       # 保留副檔名結尾，ffmpeg 靠它決定輸出容器

        opts = ["-map", "0:v:0", "-map", "0:a:0?", "-map_metadata", "0"]
        opts += (["-c:v", "copy"] if v_ok else
                 ["-c:v", "libx264", "-preset", "fast", "-crf", "20", "-pix_fmt", "yuv420p"])
        opts += ["-c:a", "copy"] if a_ok else ["-c:a", "aac", "-b:a", "192k"]
        if ext in ("mp4", "m4v", "mov"):
            opts += ["-movflags", "+faststart"]  # 讓播放器不必讀完整個檔才能開始播
        try:
            self.run_ffmpeg(path, tmp, opts)
        except FFmpegPostProcessorError as e:
            if os.path.exists(tmp):
                os.remove(tmp)
            raise PostProcessingError(f"重新編碼成 H.264 失敗：{e.msg}")

        os.replace(tmp, dst)
        if dst == path:
            return [], info
        # 換了容器：告訴後面的後處理器新檔名，舊檔交給 yt-dlp 刪除
        info["filepath"], info["ext"] = dst, ext
        return [path], info
