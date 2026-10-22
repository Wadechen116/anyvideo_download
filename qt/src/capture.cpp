#include "capture.h"

#include <QCoreApplication>
#include <cmath>
#include <QEventLoop>
#include <QThread>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QNetworkReply>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QTcpServer>
#include <QUrlQuery>
#include <QVBoxLayout>

namespace avd {

namespace {

QString headerValue(const QJsonObject& headers, const QString& name)
{
    for (auto it = headers.begin(); it != headers.end(); ++it)
        if (it.key().compare(name, Qt::CaseInsensitive) == 0) return it.value().toString();
    return {};
}

QStringList takeLinesFrom(QByteArray& buf)
{
    QStringList lines;
    int start = 0;
    for (int i = 0; i < buf.size(); ++i) {
        if (buf[i] == '\n' || buf[i] == '\r') {
            if (i > start) lines << QString::fromUtf8(buf.mid(start, i - start));
            start = i + 1;
        }
    }
    buf.remove(0, start);
    return lines;
}

constexpr int kStallSeconds = 60;

QString mb(qint64 n) { return n < 0 ? QStringLiteral("大小未知") : QString::number(n / 1048576.0, 'f', 1) + " MB"; }

}  // namespace

QString findBrowser()
{
    const QStringList roots = {qEnvironmentVariable("ProgramFiles"), qEnvironmentVariable("ProgramFiles(x86)"),
                               qEnvironmentVariable("LOCALAPPDATA")};
    const QStringList rel = {"Google/Chrome/Application/chrome.exe", "Microsoft/Edge/Application/msedge.exe"};
    for (const QString& r : rel)
        for (const QString& root : roots)
            if (!root.isEmpty() && QFileInfo::exists(root + "/" + r)) return QDir::toNativeSeparators(root + "/" + r);
    return {};
}

QString candidateKind(const QString& url, const QString& mime)
{
    const QString path = QUrl(url).path().toLower();
    const QString m = mime.toLower();
    if (path.endsWith(".m3u8") || m.contains("mpegurl")) return "stream";
    if (path.endsWith(".mpd") || m.contains("dash+xml")) return "stream";
    // 切片檔單獨拿沒有用：.ts／.m4s 是串流的一小段，要靠 m3u8／mpd 才能組回來
    if (path.endsWith(".ts") || path.endsWith(".m4s") || m.contains("mp2t")) return {};
    if (m.startsWith("audio/")) return "audio";
    if (m.startsWith("video/")) return "video";
    if (path.endsWith(".mp4") || path.endsWith(".webm") || path.endsWith(".m4a")) return "media";
    return {};
}

QUrl cleanMediaUrl(const QUrl& url)
{
    // Meta 把同一個檔拆成很多 bytestart／byteend 範圍請求；拿掉才是完整檔案
    QUrlQuery q(url);
    q.removeAllQueryItems("bytestart");
    q.removeAllQueryItems("byteend");
    QUrl u = url;
    u.setQuery(q);
    return u;
}

// ─── DevTools 連線 ──────────────────────────────────────────────────────────

CaptureSession::CaptureSession(QObject* parent) : QObject(parent), ws_(QString(), QWebSocketProtocol::VersionLatest, this)
{
    nam_.setParent(this);
    evalTimer_.setSingleShot(true);
    connect(&evalTimer_, &QTimer::timeout, this, [this] { finishQuery(); });
    timer_.setInterval(400);
    connect(&timer_, &QTimer::timeout, this, &CaptureSession::poll);
    connect(&ws_, &QWebSocket::connected, this, [this] {
        emit status(QStringLiteral("已連上瀏覽器：請在瀏覽器裡開啟並播放影片"));
        send("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", false}, {"flatten", true}});
        getTargetsId_ = nextId_;
        send("Target.getTargets");
    });
    connect(&ws_, &QWebSocket::textMessageReceived, this, &CaptureSession::onMessage);
    connect(&ws_, &QWebSocket::disconnected, this, [this] { emit status(QStringLiteral("瀏覽器已關閉，擷取結束")); });
}

bool CaptureSession::start(const QString& startUrl, QString* error)
{
    const QString browser = findBrowser();
    if (browser.isEmpty()) {
        *error = QStringLiteral("找不到 Chrome 或 Edge。");
        return false;
    }
    QTcpServer probe;  // 向系統要一個沒人用的埠
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        *error = QStringLiteral("無法取得可用的連接埠。");
        return false;
    }
    port_ = probe.serverPort();
    probe.close();

    // 獨立的設定檔：不影響你平常用的瀏覽器（Chrome 也規定偵錯模式必須用獨立設定檔）。
    // 要看登入限定的內容，在這個視窗登入一次，下次還在。
    // AVD_CAPTURE_PROFILE：測試用，改用另一個設定檔資料夾（不碰你平常的擷取設定檔，也不會被它的舊程序鎖住）
    const QString profile = QDir::toNativeSeparators(qEnvironmentVariableIsSet("AVD_CAPTURE_PROFILE") ? qEnvironmentVariable("AVD_CAPTURE_PROFILE")
                                                     : qEnvironmentVariable("APPDATA") + "/anyvideo_download/capture-profile");
    QStringList args = {QStringLiteral("--remote-debugging-port=%1").arg(port_), "--user-data-dir=" + profile,
                        "--no-first-run", "--no-default-browser-check"};
    args << QStringLiteral("about:blank");
    startUrl_ = startUrl;
    navigated_ = false;
    if (!QProcess::startDetached(browser, args)) {
        *error = QStringLiteral("無法啟動瀏覽器：") + browser;
        return false;
    }
    tries_ = 0;
    emit status(QStringLiteral("正在開啟瀏覽器…"));
    timer_.start();
    return true;
}

void CaptureSession::stop()
{
    timer_.stop();
    if (ws_.state() != QAbstractSocket::UnconnectedState) ws_.close();
}

void CaptureSession::queryPlayingVideo()
{
    videos_.clear();
    pendingEval_.clear();
    const QString expression = QStringLiteral(
        "JSON.stringify([...document.querySelectorAll('video')].map(v=>[Number.isFinite(v.duration)?v.duration:-1,v.paused?1:0,v.currentTime,v.videoWidth,v.videoHeight]))");
    for (auto it = sessions_.constBegin(); it != sessions_.constEnd(); ++it) {
        if (it.value() != QLatin1String("page") && it.value() != QLatin1String("iframe")) continue;
        pendingEval_.insert(nextId_, true);
        send("Runtime.evaluate", {{"expression", expression}, {"returnByValue", true}}, it.key());
    }
    if (pendingEval_.isEmpty() || ws_.state() != QAbstractSocket::ConnectedState) { emit playingVideo(-1); return; }
    evalTimer_.start(3000);       // 有頁面沒回應也不能一直等
}

// 正在播放的那支：沒暫停、已經播了一點的；沒有的話取播放進度最多的。長度不明（直播等）當作不知道。
void CaptureSession::finishQuery()
{
    evalTimer_.stop();
    pendingEval_.clear();
    double duration = -1;
    double bestTime = -1;
    for (const QList<double>& v : std::as_const(videos_)) {
        const bool playing = v[1] == 0 && v[2] > 0.3;
        const double score = (playing ? 1e6 : 0) + v[2];
        if (v[0] > 0 && score > bestTime) { bestTime = score; duration = v[0]; }
    }
    emit playingVideo(duration);
}

void CaptureSession::poll()
{
    if (++tries_ > 50) {
        timer_.stop();
        emit status(QStringLiteral("連不上瀏覽器的偵錯埠。請關閉所有用同一個設定檔開啟的瀏覽器視窗後再試。"));
        return;
    }
    auto* reply = nam_.get(QNetworkRequest(QUrl(QStringLiteral("http://127.0.0.1:%1/json/version").arg(port_))));
    connect(reply, &QNetworkReply::finished, this, [this, reply] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError || !timer_.isActive()) return;
        const QString url = QJsonDocument::fromJson(reply->readAll()).object().value("webSocketDebuggerUrl").toString();
        if (url.isEmpty()) return;
        timer_.stop();
        ws_.open(QUrl(url));
    });
}

void CaptureSession::send(const QString& method, const QJsonObject& params, const QString& sessionId)
{
    QJsonObject msg{{"id", nextId_++}, {"method", method}, {"params", params}};
    if (!sessionId.isEmpty()) msg["sessionId"] = sessionId;
    ws_.sendTextMessage(QString::fromUtf8(QJsonDocument(msg).toJson(QJsonDocument::Compact)));
}

void CaptureSession::onMessage(const QString& text)
{
    // 播放影片時瀏覽器會每秒送上千則 dataReceived 之類的事件；不是要用的就連 JSON 都不解析，
    // 否則介面執行緒被塞滿，視窗會卡到按不動。
    const bool wanted = text.contains(QLatin1String("\"id\":")) || text.contains(QLatin1String("Network.responseReceived\"")) ||
                        text.contains(QLatin1String("Target.attachedToTarget"));
    if (!wanted) return;
    const QJsonObject m = QJsonDocument::fromJson(text.toUtf8()).object();
    if (m.value("id").toInt() == getTargetsId_ && getTargetsId_ != 0) {
        for (const auto& t : m.value("result").toObject().value("targetInfos").toArray()) {
            const QJsonObject info = t.toObject();
            if (info.value("type").toString() == "page")
                send("Target.attachToTarget", {{"targetId", info.value("targetId").toString()}, {"flatten", true}});
        }
        return;
    }
    if (const int rid = m.value("id").toInt(); pendingEval_.contains(rid)) {      // Runtime.evaluate 的結果
        pendingEval_.remove(rid);
        const QString value = m.value("result").toObject().value("result").toObject().value("value").toString();
        for (const auto& v : QJsonDocument::fromJson(value.toUtf8()).array()) {
            QList<double> row;
            for (const auto& x : v.toArray()) row << x.toDouble(-1);
            if (row.size() >= 5) videos_ << row;
        }
        if (pendingEval_.isEmpty()) finishQuery();
        return;
    }
    const QString method = m.value("method").toString();
    const QJsonObject params = m.value("params").toObject();
    if (method == "Target.attachedToTarget") {
        const QString sid = params.value("sessionId").toString();
        sessions_.insert(sid, params.value("targetInfo").toObject().value("type").toString());
        send("Network.enable", QJsonObject(), sid);
        // 網頁裡的 worker／iframe 是另外的目標，Meta 的影片就是 worker 去抓的；每一層都要接著監聽
        send("Target.setAutoAttach", {{"autoAttach", true}, {"waitForDebuggerOnStart", false}, {"flatten", true}}, sid);
        if (!navigated_ && !startUrl_.isEmpty() && params.value("targetInfo").toObject().value("type").toString() == "page") {
            navigated_ = true;
            send("Page.navigate", {{"url", startUrl_}}, sid);
        }
    } else if (method == "Network.responseReceived") {
        const QJsonObject resp = params.value("response").toObject();
        const QString mime = resp.value("mimeType").toString();
        const QString kind = candidateKind(resp.value("url").toString(), mime);
        if (kind.isEmpty() || resp.value("status").toInt() >= 400) return;

        MediaCandidate c;
        c.url = cleanMediaUrl(QUrl(resp.value("url").toString())).toString(QUrl::FullyEncoded);
        c.mime = mime;
        c.kind = kind;
        c.referer = headerValue(resp.value("requestHeaders").toObject(), "Referer");
        // 範圍請求的 Content-Range 末尾是整個檔案的大小
        const QString range = headerValue(resp.value("headers").toObject(), "Content-Range");
        if (range.contains('/')) c.size = range.section('/', -1).toLongLong();
        else c.size = headerValue(resp.value("headers").toObject(), "Content-Length").toLongLong();
        if (c.size <= 0) c.size = -1;

        auto it = seen_.find(c.url);
        if (it == seen_.end()) {
            c.seq = int(seen_.size());
            seen_.insert(c.url, c);
            emit found(c);
        } else if (c.size > it->size || (it->referer.isEmpty() && !c.referer.isEmpty())) {
            it->size = std::max(it->size, c.size);
            if (it->referer.isEmpty()) it->referer = c.referer;
            emit found(*it);
        }
    }
}

// ─── 對話框 ─────────────────────────────────────────────────────────────────

CaptureDialog::CaptureDialog(Engine engine, QString ffmpegDir, QString outDir, QString startUrl, QWidget* parent)
    : QDialog(parent), engine_(std::move(engine)), ffmpegDir_(std::move(ffmpegDir)), outDir_(std::move(outDir))
{
    setWindowTitle(QStringLiteral("從瀏覽器擷取影片（建置 %1 %2）").arg(QStringLiteral(__DATE__), QStringLiteral(__TIME__)));
    resize(720, 420);
    auto* root = new QVBoxLayout(this);

    auto* hint = new QLabel(QStringLiteral(
        "網站解析不了、但瀏覽器能播時使用：\n"
        "① 按「開啟瀏覽器」。 ② 在開出來的瀏覽器裡播放影片（需要登入的在那裡登入一次，之後會記住）。\n"
        "③ 這裡顯示「偵測到影音」後，按「下載擷取到的影片」。程式會自動挑出影像與聲音並合併。\n"
        "有 DRM 保護的內容（Netflix 等）抓不到可用的資料。"));
    hint->setWordWrap(true);
    root->addWidget(hint);

    auto* row = new QHBoxLayout;
    urlEdit_ = new QLineEdit(startUrl);
    urlEdit_->setPlaceholderText(QStringLiteral("要先開啟的網頁網址（可留白）"));
    btnOpen_ = new QPushButton(QStringLiteral("開啟瀏覽器"));
    row->addWidget(urlEdit_, 1);
    row->addWidget(btnOpen_);
    root->addLayout(row);

    lblFound_ = new QLabel(QStringLiteral("尚未偵測到影音"));
    QFont bold = lblFound_->font();
    bold.setBold(true);
    lblFound_->setFont(bold);
    root->addWidget(lblFound_);

    btnDownload_ = new QPushButton(QStringLiteral("下載擷取到的影片"));
    btnDownload_->setEnabled(false);
    btnStop_ = new QPushButton(QStringLiteral("停止"));
    btnStop_->setEnabled(false);
    auto* btnClose = new QPushButton(QStringLiteral("關閉"));
    auto* row2 = new QHBoxLayout;
    row2->addWidget(btnDownload_);
    row2->addWidget(btnStop_);
    row2->addStretch();
    row2->addWidget(btnClose);
    root->addLayout(row2);

    bar_ = new QProgressBar;
    bar_->setRange(0, 100);
    bar_->setTextVisible(false);
    lblStatus_ = new QLabel;
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    root->addWidget(bar_);
    root->addWidget(lblStatus_);
    root->addWidget(log_, 1);

    stall_.setSingleShot(true);
    connect(&stall_, &QTimer::timeout, this, [this] {
        if (!proc_) return;
        log(QStringLiteral("第 %1 項超過 %2 秒沒有任何進度，略過這一項。").arg(index_ + 1).arg(kStallSeconds));
        killTree(*proc_);   // finished 訊號會接著下一項
    });
    connect(btnOpen_, &QPushButton::clicked, this, &CaptureDialog::openBrowser);
    connect(btnDownload_, &QPushButton::clicked, this, &CaptureDialog::downloadCaptured);
    connect(btnStop_, &QPushButton::clicked, this, &CaptureDialog::stopAll);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
}

CaptureDialog::~CaptureDialog()
{
    stopAll();
    if (session_) {
        // 對話框的子元件即將被刪除；連線關閉時送出的「已斷線」訊息不能再回頭碰它們
        QObject::disconnect(session_, nullptr, this, nullptr);
        session_->stop();
    }
}

void CaptureDialog::done(int r)
{
    stopAll();   // 關視窗時不留下還在跑的 yt-dlp
    QDialog::done(r);
}

void CaptureDialog::updateButtons()
{
    btnDownload_->setEnabled(!candidates_.isEmpty() && !proc_);
    btnStop_->setEnabled(proc_ || browserOpen_);
}

// 停止：下載中就結束 yt-dlp 整個程序樹；否則停止監聽瀏覽器
void CaptureDialog::stopAll()
{
    if (proc_) {
        cancelled_ = true;
        killTree(*proc_);
        return;   // finished 訊號會接著收尾
    }
    if (session_ && browserOpen_) {
        session_->stop();
        browserOpen_ = false;
        btnOpen_->setEnabled(true);
        lblStatus_->setText(QStringLiteral("已停止擷取（瀏覽器視窗可自行關閉）"));
    }
    updateButtons();
}

void CaptureDialog::log(const QString& text)
{
    log_->appendPlainText(QDateTime::currentDateTime().toString("HH:mm:ss  ") + text);
}

void CaptureDialog::openBrowser()
{
    if (!session_) {
        session_ = new CaptureSession(this);
        connect(session_, &CaptureSession::status, this, [this](const QString& t) { lblStatus_->setText(t); log(t); });
        connect(session_, &CaptureSession::found, this, &CaptureDialog::addCandidate);
    }
    QString err;
    if (!session_->start(urlEdit_->text().trimmed(), &err)) {
        lblStatus_->setText(err);
        log(err);
        return;
    }
    btnOpen_->setEnabled(false);
    browserOpen_ = true;
    updateButtons();
}

void CaptureDialog::addCandidate(const MediaCandidate& c)
{
    const bool isNew = !candidates_.contains(c.url);
    candidates_[c.url] = c;
    if (isNew) log(QStringLiteral("偵測到 [%1] %2 %3 %4").arg(c.kind, c.mime, mb(c.size), QUrl(c.url).host()));
    lblFound_->setText(QStringLiteral("偵測到影音：%1 個項目").arg(candidates_.size()));
    updateButtons();
}

// 自動挑最高畫質與最高音質：有串流（m3u8／mpd）就用串流（yt-dlp 自己會挑最高的）；否則把偵測到的檔案（最多 12 個）
// 全部抓下來，由 finish() 用 ffprobe 看實際的解析度、位元率與長度，再選出「正在播放的那支」裡最好的影像與聲音。
// 網址已經帶長度的（Facebook 的 efg 參數有 duration_s）先把明顯是別支影片的略過，少抓一些。
static double urlDuration(const QString& url)
{
    const QByteArray efg = QByteArray::fromBase64(QUrlQuery(QUrl(url)).queryItemValue("efg", QUrl::FullyDecoded).toUtf8(), QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const QJsonObject o = QJsonDocument::fromJson(efg).object();
    return o.contains("duration_s") ? o.value("duration_s").toDouble(-1) : -1;
}

QList<MediaCandidate> CaptureDialog::pickCandidates() const
{
    QList<MediaCandidate> all = candidates_.values();
    std::sort(all.begin(), all.end(), [](const MediaCandidate& a, const MediaCandidate& b) { return a.seq < b.seq; });
    for (const auto& c : all)
        if (c.kind == "stream") return {c};
    if (targetDuration_ > 0) {
        QList<MediaCandidate> same;
        for (const auto& c : all) {
            const double d = urlDuration(c.url);
            if (d < 0 || std::abs(d - targetDuration_) <= 1.5) same << c;
        }
        if (!same.isEmpty()) all = same;
    }
    while (all.size() > 12) all.removeLast();
    return all;
}

void CaptureDialog::downloadCaptured()
{
    targetDuration_ = -1;
    btnDownload_->setEnabled(false);
    if (session_ && browserOpen_) {
        // 先問瀏覽器：現在在播的是哪一支（長度幾秒）。Facebook 等網站會預先載入下一支，要靠長度把它們分開。
        lblStatus_->setText(QStringLiteral("讀取正在播放的影片…"));
        connect(session_, &CaptureSession::playingVideo, this, [this](double d) {
            targetDuration_ = d;
            log(d > 0 ? QStringLiteral("正在播放的影片長度：%1 秒").arg(d, 0, 'f', 1) : QStringLiteral("讀不到正在播放的影片長度，改用偵測順序判斷"));
            beginDownloads();
        }, Qt::SingleShotConnection);
        session_->queryPlayingVideo();
        return;
    }
    beginDownloads();
}

void CaptureDialog::beginDownloads()
{
    queue_ = pickCandidates();
    if (queue_.isEmpty()) return;
    QDir().mkpath(outDir_);
    files_.clear();
    index_ = 0;
    stamp_ = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    cancelled_ = false;
    btnDownload_->setEnabled(false);
    btnStop_->setEnabled(true);
    downloadNext();
}

void CaptureDialog::downloadNext()
{
    if (cancelled_ || index_ >= queue_.size()) {
        finish();
        return;
    }
    const MediaCandidate c = queue_[index_];
    lblStatus_->setText(QStringLiteral("下載第 %1/%2 項…").arg(index_ + 1).arg(queue_.size()));
    log(QStringLiteral("第 %1/%2 項：[%3] %4 %5").arg(index_ + 1).arg(queue_.size()).arg(c.kind, mb(c.size), c.url.left(110)));
    bar_->setValue(0);
    itemClock_.start();

    QStringList a = {"--ignore-config", "--newline", "--color", "never", "--no-playlist", "-q", "--progress",
                     "--no-simulate", "--retries", "10", "--fragment-retries", "10", "--socket-timeout", "30",
                     "--windows-filenames", "--merge-output-format", "mp4/mkv"};
    a << "-o" << QDir::toNativeSeparators(QStringLiteral("%1/擷取_%2_%3.%(ext)s").arg(outDir_, stamp_).arg(index_ + 1));
    a << "--print" << "after_move:AVD-FILE|%(filepath)s";
    if (!ffmpegDir_.isEmpty()) a << "--ffmpeg-location" << ffmpegDir_;
    if (!c.referer.isEmpty()) a << "--add-header" << "Referer:" + c.referer;
    // fbcdn 這類 CDN 對 yt-dlp 預設的連線特徵會限速（實測同一個 0.6 MB 檔案：預設 86 秒，偽裝成 Chrome 只要 2.5 秒，
    // curl 0.8 秒），所以先以 Chrome 身分連線；沒裝 curl_cffi 時 yt-dlp 會立刻報錯，那一項改用預設方式重試。
    if (!noImpersonate_) a << "--impersonate" << "chrome";
    a << "--" << c.url;

    buffer_.clear();
    proc_ = new QProcess(this);
    configureProcess(*proc_);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, [this] {
        stall_.start(kStallSeconds * 1000);   // 有輸出就重新計時
        buffer_ += proc_->readAllStandardOutput();
        static const QRegularExpression pct(R"(\[download\]\s+([\d.]+)%)");
        for (const QString& line : takeLinesFrom(buffer_)) {
            if (line.startsWith("AVD-FILE|")) {
                files_ << line.mid(9).trimmed();
                log(QStringLiteral("第 %1 項完成（%2 秒）").arg(index_ + 1).arg(itemClock_.elapsed() / 1000));
            } else if (const auto m = pct.match(line); m.hasMatch()) {
                bar_->setValue(int(m.captured(1).toDouble()));
            } else if (line.startsWith("ERROR") || line.startsWith("WARNING")) {
                log(line);
            }
        }
    });
    connect(proc_, &QProcess::finished, this, [this](int code, QProcess::ExitStatus) {
        stall_.stop();
        proc_->deleteLater();
        proc_ = nullptr;
        if (code != 0 && !cancelled_ && !noImpersonate_ && itemClock_.elapsed() < 15000 && !impersonateFailed_) {
            // 很快就失敗：多半是這個 yt-dlp 沒有偽裝元件。同一項改用預設方式再試一次
            impersonateFailed_ = true;
            noImpersonate_ = true;
            log(QStringLiteral("以 Chrome 身分連線失敗，改用預設方式重試這一項…"));
            downloadNext();
            return;
        }
        if (code != 0 && !cancelled_) log(QStringLiteral("第 %1 項下載失敗（結束代碼 %2）").arg(index_ + 1).arg(code));
        impersonateFailed_ = false;
        ++index_;
        downloadNext();
    });
    connect(proc_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        log(QStringLiteral("無法執行 yt-dlp"));
        proc_->deleteLater();
        proc_ = nullptr;
        cancelled_ = true;
        finish();
    });
    proc_->start(engine_.program, engine_.args(a));
    stall_.start(kStallSeconds * 1000);
    updateButtons();
}

void CaptureDialog::finish()
{
    if (cancelled_) {
        bar_->setValue(0);
        lblStatus_->setText(QStringLiteral("已停止。未完成的 .part 暫存檔留在儲存位置。"));
        log(lblStatus_->text());
        updateButtons();
        return;
    }
    bar_->setValue(100);
    updateButtons();
    files_.removeIf([](const QString& f) { return !QFileInfo::exists(f); });
    if (files_.isEmpty()) {
        lblStatus_->setText(QStringLiteral("沒有下載到任何檔案，原因見下方紀錄。"));
        return;
    }

    // 每個下載到的檔案用 ffprobe 量：有沒有影像／聲音、解析度、長度、位元率。
    struct Info { QString path; bool video = false, audio = false; int w = 0, h = 0; double dur = -1; qint64 size = 0; double pixels() const { return double(w) * h; } double bitrate() const { return dur > 0 ? size * 8.0 / dur : 0; } };
    QList<Info> infos;
    for (const QString& f : files_) {
        Info i;
        i.path = f;
        i.size = QFileInfo(f).size();
        i.video = true;
        if (!ffmpegDir_.isEmpty()) {
            QProcess p;
            configureProcess(p);
            p.start(ffmpegDir_ + "/ffprobe.exe", {"-v", "error", "-print_format", "json", "-show_entries", "stream=codec_type,width,height:format=duration", f});
            p.waitForFinished(30000);
            const QJsonObject o = QJsonDocument::fromJson(p.readAllStandardOutput()).object();
            i.video = i.audio = false;
            for (const auto& sv : o.value("streams").toArray()) {
                const QJsonObject s = sv.toObject();
                if (s.value("codec_type").toString() == "video" && !i.video) { i.video = true; i.w = s.value("width").toInt(); i.h = s.value("height").toInt(); }
                else if (s.value("codec_type").toString() == "audio") i.audio = true;
            }
            bool ok = false;
            i.dur = o.value("format").toObject().value("duration").toString().toDouble(&ok);
            if (!ok) i.dur = -1;
        }
        infos << i;
    }
    // 只留「正在播放的那支」：長度和頁面上的 <video> 最接近的那一組。同一支影片的各種畫質、影像與聲音，長度只差幾百分之一秒；
    // 預先載入的下一支影片長度不同，即使只差 0.4 秒也不能混進來（實測：10.4 秒的影片旁邊有一支 10.0 秒的，容許 ±0.6 秒就會選錯）。
    // 所以不是「在容許範圍內就算」，而是先找出最接近的差距，只留和它差不到 0.15 秒的。
    // 一支都找不到（頁面讀不到長度）就不篩，照全部挑。
    QList<Info> pool = infos;
    for (const Info& i : infos) log(QStringLiteral("  %1：%2%3 %4 秒 %5 KB").arg(QFileInfo(i.path).fileName(), i.video ? QStringLiteral("影像 %1×%2 ").arg(i.w).arg(i.h) : QString(), i.audio ? QStringLiteral("聲音") : QString()).arg(i.dur, 0, 'f', 2).arg(i.size / 1024));
    if (targetDuration_ > 0) {
        double bestDiff = 1e9;
        for (const Info& i : infos) if (i.dur > 0) bestDiff = std::min(bestDiff, std::abs(i.dur - targetDuration_));
        QList<Info> same;
        if (bestDiff <= std::max(0.6, targetDuration_ * 0.05))
            for (const Info& i : infos) if (i.dur > 0 && std::abs(i.dur - targetDuration_) <= bestDiff + 0.15) same << i;
        if (!same.isEmpty()) pool = same;
        log(QStringLiteral("長度最接近正在播放影片的檔案：%1／%2 個").arg(same.size()).arg(infos.size()));
    }
    // 最高畫質＝解析度最大、同解析度取位元率最高；最高音質＝位元率最高。
    auto betterVideo = [](const Info& a, const Info& b) { return a.pixels() != b.pixels() ? a.pixels() > b.pixels() : a.bitrate() > b.bitrate(); };
    Info wholeInfo, bestV, bestA;
    for (const Info& i : pool) {
        if (i.video && i.audio) { if (wholeInfo.path.isEmpty() || betterVideo(i, wholeInfo)) wholeInfo = i; }
        else if (i.video) { if (bestV.path.isEmpty() || betterVideo(i, bestV)) bestV = i; }
        else if (i.audio) { if (bestA.path.isEmpty() || i.bitrate() > bestA.bitrate()) bestA = i; }
    }
    // 影像與聲音分開的組合，和影像聲音在一起的檔案比：解析度較高（或一樣高）的組合勝出，因為聲音也是挑最高的。
    const bool pairWins = !bestV.path.isEmpty() && !bestA.path.isEmpty() && (wholeInfo.path.isEmpty() || bestV.pixels() >= wholeInfo.pixels());
    QString video = pairWins ? bestV.path : QString(), audio = pairWins ? bestA.path : QString(), whole_ = pairWins ? QString() : wholeInfo.path;
    if (pairWins) log(QStringLiteral("選用：影像 %1×%2、聲音 %3 kbps").arg(bestV.w).arg(bestV.h).arg(qint64(bestA.bitrate() / 1000)));
    else if (!whole_.isEmpty()) log(QStringLiteral("選用：影像聲音在一起的檔案 %1×%2").arg(wholeInfo.w).arg(wholeInfo.h));
    else if (!bestV.path.isEmpty()) { video = bestV.path; log(QStringLiteral("只找到影像（%1×%2），沒有找到聲音").arg(bestV.w).arg(bestV.h)); }
    const QString whole = whole_;
    auto report = [this](const QString& path) {
        lblStatus_->setText(QStringLiteral("完成：") + QDir::toNativeSeparators(path));
        log(lblStatus_->text());
    };
    if (!whole.isEmpty()) {
        for (const QString& f : files_) if (f != whole) QFile::remove(f);
        report(whole);
        return;
    }
    if (video.isEmpty() || audio.isEmpty() || ffmpegDir_.isEmpty()) {
        report(video.isEmpty() ? files_.first() : video);   // 只有單一種串流，或沒有 ffmpeg 可合併
        return;
    }
    const QString out = QStringLiteral("%1/擷取_%2.mp4").arg(outDir_, stamp_);
    QProcess ff;
    configureProcess(ff);
    ff.start(ffmpegDir_ + "/ffmpeg.exe", {"-y", "-loglevel", "error", "-i", video, "-i", audio, "-map", "0:v:0",
                                          "-map", "1:a:0", "-c", "copy", "-shortest", "-movflags", "+faststart", out});
    ff.waitForFinished(600000);
    if (ff.exitCode() != 0 || !QFileInfo::exists(out)) {
        log(QStringLiteral("合併失敗：") + QString::fromUtf8(ff.readAllStandardError()).trimmed().right(300));
        lblStatus_->setText(QStringLiteral("影像與聲音已分別下載，但合併失敗，檔案留在儲存位置。"));
        return;
    }
    for (const QString& f : files_) QFile::remove(f);
    report(out);
}

bool CaptureDialog::selfTestLive(const QString& url, const QString& png)
{
    show();
    urlEdit_->setText(url);
    btnOpen_->click();
    for (int i = 0; i < 300 && candidates_.isEmpty(); ++i) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(50);
    }
    const int extra = qEnvironmentVariableIntValue("AVD_WAIT_SECS");
    for (int i = 0; i < (extra > 0 ? extra * 20 : 60) && !candidates_.isEmpty(); ++i) {   // 再等幾秒，讓影像與聲音都出現
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        QThread::msleep(50);
    }
    bool ok = btnDownload_->isEnabled();
    if (ok) {   // 一路測到下載完成
        btnDownload_->click();
        for (int i = 0; i < 600 && (proc_ || lblStatus_->text().startsWith(QStringLiteral("下載第")) || lblStatus_->text().startsWith(QStringLiteral("讀取"))); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            QThread::msleep(50);
        }
        ok = lblStatus_->text().startsWith(QStringLiteral("完成："));
    }
    grab().save(png);
    QFile f(png + ".txt");
    if (f.open(QIODevice::WriteOnly)) f.write(log_->toPlainText().toUtf8());
    return ok;
}

}  // namespace avd
