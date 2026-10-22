// 下載核心：找 yt-dlp、組命令列參數、執行下載／解析／更新。與介面無關。
//
// 規則與 py/anyvideo_download.py 的 build_ydl_options() 相同，只是 Python 版直接呼叫
// yt-dlp 函式庫，這裡呼叫 yt-dlp 命令列。Python 版自己插入的 EnsureH264PP，這裡由
// yt-dlp 外掛（shared/yt-dlp-plugins）提供。
#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <QProcess>
#include <QSet>
#include <QString>
#include <QStringList>

#include "platform.h"
#include "settings.h"

namespace avd {

struct Engine {
    enum class Kind { None, Exe, PyModule };
    Kind kind = Kind::None;
    QString program;      // yt-dlp.exe 或 py.exe
    QStringList prefix;   // PyModule 時是 -3 -m yt_dlp
    QString version;

    bool ok() const { return kind != Kind::None; }
    QStringList args(const QStringList& rest) const { return prefix + rest; }
};

// 依序找：程式目錄的 yt-dlp.exe → PATH 上的 yt-dlp.exe → py -3 -m yt_dlp。
// 每個候選都實際跑一次 --version，會花一兩秒，不要在介面執行緒呼叫。
Engine detectEngine();

QString findFfmpegDir();   // 找不到回傳空字串
bool hasDeno();
QString pluginDir();       // 程式目錄\yt-dlp-plugins
// 外掛確實在 pluginDir() 底下時回傳 pluginDir()，否則回傳空字串。
// 資料夾不存在時不能把它交給 --plugin-dirs：yt-dlp 會丟出
// 「Invalid plugin directory」直接結束，每一個網址都下載失敗。
QString pluginDirIfPresent();
// 這組設定會不會用到 EnsureH264 外掛（相容優先，或輸出 mov）
bool needsEnsureH264(const Settings& s);

// 失敗後「換個方式再試」的調整，見 planRetry()。預設值就是第一次嘗試。
struct RetryTweaks {
    bool relaxFormat = false;   // 放掉畫質／編碼排序，用寬鬆的格式選擇
    bool impersonate = false;   // 以 Chrome 瀏覽器身分連線（需要 curl_cffi）
};

QStringList buildDownloadArgs(const Settings& s, const QString& ffmpegDir, const QString& pluginDir,
                              const QString& url, const RetryTweaks& tweaks = {});
QStringList buildProbeArgs(const Settings& s, const QString& url);

// 錯誤訊息分類（與 py 版 classify_error 相同）：drm disk bot login geo impersonate format
// network ffmpeg update，認不出來是 unknown。
QString classifyError(const QString& text);
// 失敗原因 → 下一步該怎麼做；沒有建議回傳空字串。
QString errorHint(const QString& kind, const Settings& s);
// 這類失敗值得換個方式再試嗎？值得就回傳 true 並填入 tweaks／message；
// tried 記錄已試過的類別，每種辦法只試一次。
bool planRetry(const QString& kind, QSet<QString>& tried, RetryTweaks& tweaks, QString& message);
// yt-dlp 版本（2026.08.19）距今幾天；解析不了回傳 -1。
int coreAgeDays(const QString& version);

QString fmtBytes(double n);   // n <= 0 表示未知
QString fmtEta(double sec);   // sec < 0 表示未知

// 子程序共同設定：UTF-8 輸出、不彈主控台視窗
void configureProcess(QProcess& p);
// yt-dlp 會再開 ffmpeg，只殺 yt-dlp 會留下孤兒 ffmpeg，所以整棵程序樹一起結束
void killTree(QProcess& p);

// 依序下載多個網址，每個網址一個 yt-dlp 程序。
// buildDownloadArgs 的 pluginDir 傳空字串表示不帶 --plugin-dirs。
class DownloadJob : public QObject {
    Q_OBJECT
public:
    DownloadJob(QStringList urls, Settings s, Engine engine, QString ffmpegDir, QObject* parent = nullptr);
    void start();
    void cancel();
    // 失敗了、而且這類網站改用「從瀏覽器擷取」常常有效的網址（抖音、小紅書、TikTok）
    QStringList captureCandidates() const { return captureUrls_; }

signals:
    void log(const QString& text, const QString& tag);
    void item(int index, int count, const QString& url);
    void progress(double pct, const QString& text);  // pct < 0：沒有百分比
    void status(const QString& text);                // 後處理中
    void done(int ok, int fail, bool cancelled);

private:
    void runNext();
    void startAttempt(const QString& url);
    void startUrl(const QString& url);
    void onOutput();
    void onFinished(int exitCode, QProcess::ExitStatus st);
    void handleLine(const QString& line);
    void handleDownloadProgress(const QStringList& f);

    QStringList urls_;
    Settings settings_;
    Engine engine_;
    QString ffmpegDir_;
    QProcess* proc_ = nullptr;
    QByteArray buffer_;
    QElapsedTimer lastPost_;
    QString url_;
    LinkResolver* resolver_ = nullptr;
    QStringList captureUrls_;
    QStringList errors_;     // 這一次嘗試印出的 ERROR 行，失敗時用來判斷原因
    QSet<QString> tried_;
    RetryTweaks tweaks_;
    int index_ = 0;
    int ok_ = 0;
    int fail_ = 0;
    bool cancelled_ = false;
};

// 只解析、不下載：看標題、長度、可用的畫質。
class ProbeJob : public QObject {
    Q_OBJECT
public:
    ProbeJob(QString url, Settings s, Engine engine, QObject* parent = nullptr);
    void start();

signals:
    void log(const QString& text, const QString& tag);
    void done(bool ok, const QString& text);

private:
    void startProcess();
    QString url_;
    Settings settings_;
    Engine engine_;
};

// 更新（或安裝）yt-dlp。網站改版時下載核心常需要跟著更新。
class UpdateJob : public QObject {
    Q_OBJECT
public:
    explicit UpdateJob(Engine engine, QObject* parent = nullptr);
    void start();

signals:
    void log(const QString& text, const QString& tag);
    void done(int exitCode);

private:
    Engine engine_;
};

}  // namespace avd
