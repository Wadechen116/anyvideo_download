// 格式選項與使用者設定。
//
// 選項清單與 py/anyvideo_download.py、csharp/Options.cs 三邊一致；設定檔也是同一個
// （%APPDATA%\anyvideo_download\settings.json），換版本使用時設定會沿用。
#pragma once

#include <QList>
#include <QSet>
#include <QString>
#include <QStringList>

namespace avd {

inline const QString kAppName = QStringLiteral("AnyVideo Download");
inline const QString kAppVersion = QStringLiteral("1.0.0");

// 顯示文字 + 存進設定檔的值。設定檔存值而不存文字，以後改顯示文字時舊設定才不會失效。
struct Choice {
    QString label;
    QString value;
};

inline const QStringList kVideoContainers = {"mp4", "mkv", "webm", "mov", "avi"};

// value 空字串 = 不限制高度
inline const QList<Choice> kVideoQualities = {
    {QStringLiteral("最佳"), ""}, {"2160p", "2160"}, {"1440p", "1440"}, {"1080p", "1080"},
    {"720p", "720"},             {"480p", "480"},   {"360p", "360"},
};

inline const QList<Choice> kPriorities = {
    {QStringLiteral("畫質優先 — 最高解析度，可能是 AV1／VP9"), "quality"},
    {QStringLiteral("相容優先 — H.264，到哪都能播"), "compat"},
    // 不轉檔：網站提供的最高畫質影像與最高音質聲音，原封不動封裝（串流複製）。能放進 mp4 才用 mp4，否則 mkv。
    // 與來源逐格相同，沒有任何損耗；代價是 AV1／VP9／Opus 在舊電視、舊手機、部分剪輯軟體上不一定能播。
    {QStringLiteral("原始品質 — 不轉檔，無損耗（常是 mkv）"), "original"},
};

// 相容優先時保證輸出 H.264 的容器。webm 規格裝不下 H.264，avi 另有固定編碼。
inline const QSet<QString> kCompatH264Containers = {"mp4", "mkv", "mov"};

inline const QStringList kAudioFormats = {"mp3", "m4a", "opus", "ogg", "flac", "wav"};
// 音訊的「原始格式」：不轉檔，存成網站原本的聲音（opus、m4a…）。設定檔存 "best"（yt-dlp --audio-format 的寫法）。
inline const QString kAudioOriginalLabel = QStringLiteral("原始（不轉檔）");
inline const QString kAudioOriginalValue = QStringLiteral("best");
inline const QStringList kAudioBitrates = {"320", "256", "192", "128", "96"};
inline const QSet<QString> kLosslessAudio = {"flac", "wav"};

// 能嵌入封面／字幕的容器。不支援時直接停用勾選框，而不是讓使用者勾了才失敗。
inline const QSet<QString> kThumbOk = {"mp4", "mkv", "mov", "mp3", "m4a", "opus", "ogg", "flac"};
inline const QSet<QString> kSubsOk = {"mp4", "mkv", "webm", "mov"};

// value 空字串 = 不使用
inline const QList<Choice> kBrowsers = {
    {QStringLiteral("不使用"), ""}, {"Firefox", "firefox"}, {"Edge", "edge"}, {"Chrome", "chrome"},
};

// 後處理器名稱 -> 狀態列文字
QString ppLabel(const QString& name);

// 在 choices 裡用 label 找 value；找不到回傳 fallback
QString valueOf(const QList<Choice>& choices, const QString& label, const QString& fallback = {});
QString labelOf(const QList<Choice>& choices, const QString& value, const QString& fallback = {});
bool hasLabel(const QList<Choice>& choices, const QString& label);

QString settingsPath();

struct Settings {
    QString outDir;
    QString mode = "video";      // "video" 或 "audio"
    QString container = "mp4";
    QString quality = QStringLiteral("最佳");
    QString priority = "quality";  // "quality" 或 "compat"
    QString audioFormat = "mp3";
    QString audioBitrate = "192";
    bool playlist = false;
    bool embedMeta = true;
    bool embedThumb = false;
    bool embedSubs = false;
    QString browser = QStringLiteral("不使用");

    Settings();
    static Settings load();                       // 讀預設位置
    static Settings loadFrom(const QString& path);  // 讀不到就回傳預設值
    void save() const;
};

}  // namespace avd
