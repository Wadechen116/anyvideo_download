// 從瀏覽器擷取：開一個 Chrome／Edge（偵錯模式），使用者照常播放，程式透過 Chrome DevTools
// Protocol 監聽網路請求，找出播放器實際載入的影音網址（m3u8、mpd、mp4…），交給 yt-dlp 下載。
// 用在網站頁面解析不了、但瀏覽器能播的情況（例如 Meta Reels）。有 DRM 保護的內容不適用。
#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QWebSocket>

#include "engine.h"

class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;

namespace avd {

struct MediaCandidate {
    QString url;       // 已去掉 bytestart／byteend（Meta 把一個檔切成很多段請求）
    QString mime;
    QString referer;
    QString kind;      // stream（m3u8／mpd）、video、audio、media
    qint64 size = -1;  // 未知為 -1
    int seq = 0;       // 第幾個被偵測到（越小越早）
};

QString findBrowser();                                   // Chrome → Edge；找不到回傳空字串
QString candidateKind(const QString& url, const QString& mime);  // 不是影音回傳空字串
QUrl cleanMediaUrl(const QUrl& url);

// 單一瀏覽器的 DevTools 連線，只負責找出影音網址。
class CaptureSession : public QObject {
    Q_OBJECT
public:
    explicit CaptureSession(QObject* parent = nullptr);
    bool start(const QString& startUrl, QString* error);
    void stop();   // 結束監聽（瀏覽器視窗不關）
    // 問頁面裡正在播放的 <video>：長度幾秒。用來分辨「使用者在看的那支」和預先載入的下一支。
    void queryPlayingVideo();
    bool lastQueryPlaying() const { return lastPlaying_; }   // 上一次 queryPlayingVideo 讀到的那支是不是真的在播（沒暫停、播了一點）
    void closeBrowser();   // 關掉整個瀏覽器（自動擷取成功後用；獨立設定檔的登入資訊會留著）

signals:
    void playingVideo(double durationSeconds);   // 讀不到時是 -1
    void found(const avd::MediaCandidate& c);  // 新的，或同一個網址的資訊更新了
    void status(const QString& text);

private:
    void poll();
    void send(const QString& method, const QJsonObject& params = QJsonObject(), const QString& sessionId = {});
    void onMessage(const QString& text);

    QNetworkAccessManager nam_;
    QWebSocket ws_;
    QTimer timer_;
    int port_ = 0;
    int tries_ = 0;
    int nextId_ = 1;
    int getTargetsId_ = 0;
    QString startUrl_;      // 連上之後才導向，避免頁面在開始監聽前就載完而漏掉請求
    bool navigated_ = false;
    bool lastPlaying_ = false;
    QHash<QString, MediaCandidate> seen_;
    QHash<QString, QString> sessions_;           // sessionId -> 目標類型（page、iframe、worker…）
    QHash<int, bool> pendingEval_;               // 還在等結果的 Runtime.evaluate
    QList<QList<double>> videos_;                // 收到的 <video>：[長度, 暫停(1/0), 目前時間, 寬, 高]
    QTimer evalTimer_;
    void finishQuery();
};

class CaptureDialog : public QDialog {
    Q_OBJECT
public:
    CaptureDialog(Engine engine, QString ffmpegDir, QString outDir, QString startUrl, QWidget* parent = nullptr);
    ~CaptureDialog() override;

    // 測試用：真的開瀏覽器載入 url，等偵測到影音，回報「下載擷取到的影片」是否可按，並存畫面
    bool selfTestLive(const QString& url, const QString& png);

protected:
    void done(int r) override;

private:
    void openBrowser();
    void addCandidate(const MediaCandidate& c);
    QList<MediaCandidate> pickCandidates() const;
    void downloadCaptured();
    void beginDownloads();
    void startAuto();
    void autoTick();
    void autoVerify(const QString& path);
    void autoFallback(const QString& msg);
    void startItem(int i);
    void pump();
    void updateProgress();
    void finish();
    void stopAll();
    void updateButtons();
    void log(const QString& text);

    Engine engine_;
    QString ffmpegDir_;
    QString outDir_;
    CaptureSession* session_ = nullptr;
    QHash<QString, MediaCandidate> candidates_;

    QLineEdit* urlEdit_ = nullptr;
    QPushButton* btnOpen_ = nullptr;
    QPushButton* btnAuto_ = nullptr;
    QPushButton* btnDownload_ = nullptr;
    QPushButton* btnStop_ = nullptr;
    QLabel* lblFound_ = nullptr;
    QProgressBar* bar_ = nullptr;
    QLabel* lblStatus_ = nullptr;
    QPlainTextEdit* log_ = nullptr;

    QList<MediaCandidate> queue_;
    QString stamp_;
    struct Item {
        QProcess* proc = nullptr;
        QByteArray buf;
        QTimer* stall = nullptr;       // 一段時間沒有任何輸出就當作卡住，略過這一項
        QElapsedTimer clock;
        int pct = 0;
        bool retried = false;          // 已經因為偽裝元件失敗而改用預設方式重試過
        QString file;
    };
    QList<Item*> items_;               // 和 queue_ 一一對應；合併時的挑選順序不受完成先後影響
    // 自動擷取：偵測到正在播放的影片就鎖定長度 → 等影音到齊 → 下載 → 長度比對 → 成功就關瀏覽器
    QTimer autoTimer_;
    QElapsedTimer autoClock_, settleClock_;
    int autoState_ = 0;                // 0 沒在自動、1 等正在播放的影片、2 等影音到齊
    bool autoRun_ = false;             // 這一輪下載是自動擷取發起的
    bool queryInFlight_ = false;
    qsizetype settleCount_ = 0;
    int nextIndex_ = 0;
    int running_ = 0;                  // 正在下載的項目數
    int doneCount_ = 0;
    bool cancelled_ = false;
    double targetDuration_ = -1;   // 使用者正在看的那支影片的長度（秒）；-1 = 不知道
    bool browserOpen_ = false;
    bool noImpersonate_ = false;      // 這個 yt-dlp 不能偽裝成瀏覽器：之後都不帶 --impersonate
};

}  // namespace avd
