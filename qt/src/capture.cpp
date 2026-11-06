#include "capture.h"
#include "platform.h"

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
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTemporaryDir>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
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
constexpr int kAutoWaitSeconds = 30;      // 自動擷取：等到「有影音、而且有一支正在播放」的時間上限；超過就交給使用者手動操作（多半要登入）
constexpr int kAutoSettleMs = 2000;       // 偵測到的影音檔案這麼久沒再增加，就當作影像與聲音都到齊了
constexpr int kParallelDownloads = 4;   // 同時下載幾個（太多會被 CDN 當成異常連線）

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
    if (m.startsWith("image/") && url.contains("aweme-images")) return "slide";   // 抖音圖文貼文的圖片（封面、頭像不帶這個字）
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
    seen_.clear();          // 新的一輪：上一輪的偵測結果與頁面連線都不算
    sessions_.clear();
    pendingEval_.clear();
    // 抖音網址帶影片 ID（/video/ID、?modal_id=ID）：自動擷取只認這支。帳號開著自動連播時，短影片播完頁面就換下一支，
    // 網址的 modal_id 和「目前這一篇」容器都會跟著變，所以在開始時就固定下來。
    startTargetId_ = douyinVideoId(QUrl(startUrl.trimmed()));
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

void CaptureSession::closeBrowser()
{
    send("Browser.close");
    ws_.flush();
    QTimer::singleShot(300, this, [this] { stop(); });
}

void CaptureSession::queryPlayingVideo(const QString& targetId)
{
    videos_.clear();
    pendingEval_.clear();
    queryTargetId_ = targetId;
    // 除了 <video> 的狀態，也一併問頁面「目前這一篇」是不是抖音圖文貼文：圖片依頁面順序、背景音樂網址。
    // 音樂本身也是個 <video>，頁面還會預載別篇的影片與圖片，只看網路請求或 <video> 會抓錯。
    // 「這一篇」的容器：有指定抖音影片 ID 時是帶 data-e2e-vid=ID 的容器（不管它是不是正在播放，播完換下一支後仍留在頁面上）；
    // 沒指定時是 data-e2e=feed-active-video（頁面目前顯示的那篇）。
    lastPost_ = QJsonObject();
    QString expression = QStringLiteral(R"JS(JSON.stringify((()=>{
const T='%1';const els=T?[...document.querySelectorAll('[data-e2e-vid="'+T+'"]')]:[];
const r=T?(els.find(e=>e.querySelector('video'))||els.find(e=>e.querySelector('img'))||null):document.querySelector('[data-e2e="feed-active-video"]');let post={};
// 第 6 欄：在這一篇容器裡是 1（容器裡的背景音樂 <video> 是 2）、不在是 0、頁面沒有這個容器是 -1（有指定 ID 卻找不到容器也是 0：一支都不算，繼續等）
const v=[...document.querySelectorAll('video')].map(v=>[Number.isFinite(v.duration)?v.duration:-1,v.paused?1:0,v.currentTime,v.videoWidth,v.videoHeight,r?(r.contains(v)?(/ies-music|\.mp3/.test(v.currentSrc||v.src||'')?2:1):0):(T?0:-1)]);
if(r){const key=u=>{try{return new URL(u).pathname.split('~')[0]}catch(e){return ''}};const seen=new Set(),slides=[];
r.querySelectorAll('img').forEach(i=>{if(/aweme-images/.test(i.src)&&!seen.has(key(i.src))){seen.add(key(i.src));slides.push({key:key(i.src),src:i.src})}});
const m=[...r.querySelectorAll('video,audio')].map(v=>v.currentSrc||v.src||'').find(u=>/^https?:/.test(u))||'';post={slides:slides,music:m}}
return {v:v,post:post}})()))JS");
    static const QRegularExpression digits(QStringLiteral("^\\d*$"));
    expression = expression.arg(digits.match(targetId).hasMatch() ? targetId : QString());
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
    lastPlaying_ = false;
    lastTargetHit_ = false;
    bool haveRoot = false;
    for (const QList<double>& v : std::as_const(videos_)) if (v.size() > 5 && v[5] >= 0) haveRoot = true;
    for (const QList<double>& v : std::as_const(videos_)) {
        if (haveRoot && !(v.size() > 5 && v[5] == 1)) continue;
        const bool playing = v[1] == 0 && v[2] > 0.3;
        const double score = (playing ? 1e6 : 0) + v[2];
        if (v[0] > 0 && score > bestTime) { bestTime = score; duration = v[0]; lastPlaying_ = playing; }
    }
    lastTargetHit_ = !queryTargetId_.isEmpty() && duration > 0;   // 找到指定 ID 那支的長度（不一定還在播：可能已播完換下一支）
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
        const QJsonObject reply = QJsonDocument::fromJson(value.toUtf8()).object();
        for (const auto& v : reply.value("v").toArray()) {
            QList<double> row;
            for (const auto& x : v.toArray()) row << x.toDouble(-1);
            if (row.size() >= 5) videos_ << row;
        }
        if (lastPost_.isEmpty() && !reply.value("post").toObject().value("slides").toArray().isEmpty()) lastPost_ = reply.value("post").toObject();
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
    btnAuto_ = new QPushButton(QStringLiteral("自動擷取"));
    btnOpen_ = new QPushButton(QStringLiteral("開啟瀏覽器"));
    row->addWidget(urlEdit_, 1);
    row->addWidget(btnAuto_);
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

    autoTimer_.setInterval(500);
    connect(&autoTimer_, &QTimer::timeout, this, &CaptureDialog::autoTick);
    connect(btnAuto_, &QPushButton::clicked, this, &CaptureDialog::startAuto);
    connect(btnOpen_, &QPushButton::clicked, this, &CaptureDialog::openBrowser);
    connect(btnDownload_, &QPushButton::clicked, this, &CaptureDialog::downloadCaptured);
    connect(btnStop_, &QPushButton::clicked, this, &CaptureDialog::stopAll);
    connect(btnClose, &QPushButton::clicked, this, &QDialog::accept);
    if (!startUrl.isEmpty()) QTimer::singleShot(300, this, &CaptureDialog::startAuto);   // 帶著網址開啟：直接自動擷取
}

CaptureDialog::~CaptureDialog()
{
    stopAll();
    qDeleteAll(items_);
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
    btnDownload_->setEnabled(!candidates_.isEmpty() && running_ == 0);
    btnStop_->setEnabled(running_ > 0 || browserOpen_ || autoState_ != 0);
    btnAuto_->setEnabled(!browserOpen_ && running_ == 0);
}

// 停止：下載中就結束 yt-dlp 整個程序樹；否則停止監聽瀏覽器
void CaptureDialog::stopAll()
{
    autoTimer_.stop();
    autoState_ = 0;
    autoRun_ = false;
    if (running_ > 0) {
        cancelled_ = true;
        for (Item* it : items_)
            if (it->proc) killTree(*it->proc);
        return;   // 每個 finished 訊號會接著收尾
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
    candidates_.clear();    // 新的一輪不混進上一輪偵測到的影音
    lblFound_->setText(QStringLiteral("尚未偵測到影音"));
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

// 抖音圖文貼文：沒有任何影像／串流，只有圖片（slide）加音樂 → 依偵測順序的圖片與音樂。
// 同一張圖會以不同尺寸（~tplv-… 後綴不同）、不同簽章重複請求，用檔案路徑 ~ 之前的部分當身分。
bool CaptureDialog::slideshowParts(QList<MediaCandidate>* slides, MediaCandidate* music) const
{
    QList<MediaCandidate> all = candidates_.values();
    std::sort(all.begin(), all.end(), [](const MediaCandidate& a, const MediaCandidate& b) { return a.seq < b.seq; });
    // 頁面說目前這一篇是圖文貼文：用頁面上的圖片網址（高解析版、簽章完整）與音樂，不管偵測到什麼影片（多半是預載的別篇）
    if (const QJsonArray ps = post_.value("slides").toArray(); !ps.isEmpty()) {
        QString referer = QStringLiteral("https://www.douyin.com/");
        for (const auto& c : all) if (!c.referer.isEmpty()) { referer = c.referer; break; }
        MediaCandidate m;
        const QString musicUrl = post_.value("music").toString();
        if (!musicUrl.isEmpty()) { m.url = musicUrl; m.kind = "audio"; m.referer = referer; }
        else for (const auto& c : all) if (c.kind == "audio" || c.kind == "media") { m = c; break; }
        if (m.url.isEmpty()) return false;
        if (slides) {
            slides->clear();
            for (const auto& sv : ps) {
                MediaCandidate c;
                c.url = sv.toObject().value("src").toString();
                c.kind = "slide";
                c.mime = "image/webp";
                c.referer = referer;
                *slides << c;
            }
        }
        if (music) *music = m;
        return true;
    }
    QList<MediaCandidate> imgs;
    QSet<QString> keys;
    bool haveMusic = false;
    MediaCandidate m;
    for (const auto& c : all) {
        if (c.kind == "stream" || c.kind == "video") return false;
        if (c.kind == "slide") {
            const QString key = QUrl(c.url).path().section('~', 0, 0);
            if (keys.contains(key)) {
                for (auto& old : imgs)
                    if (QUrl(old.url).path().section('~', 0, 0) == key && c.size > old.size) { old.url = c.url; old.size = c.size; }
            } else {
                keys.insert(key);
                imgs << c;
            }
        } else if (!haveMusic && (c.kind == "audio" || c.kind == "media")) {
            m = c;
            haveMusic = true;
        }
    }
    if (imgs.isEmpty() || !haveMusic) return false;
    if (slides) *slides = imgs;
    if (music) *music = m;
    return true;
}

// 下載圖片、和音樂合成 mp4（圖片平均分配到音樂長度）。回傳結果路徑；失敗回傳空字串。
QString CaptureDialog::makeSlideshow(const QList<MediaCandidate>& slides, const QString& audio)
{
    if (ffmpegDir_.isEmpty()) {
        log(QStringLiteral("找不到 ffmpeg，無法合成圖文影片。"));
        return {};
    }
    QTemporaryDir tmp;
    if (!tmp.isValid()) return {};
    QNetworkAccessManager nam;
    QStringList files;
    lblStatus_->setText(QStringLiteral("下載 %1 張圖片…").arg(slides.size()));
    for (int i = 0; i < slides.size(); ++i) {
        QNetworkRequest req{QUrl(slides[i].url)};
        req.setRawHeader("Referer", (slides[i].referer.isEmpty() ? QStringLiteral("https://www.douyin.com/") : slides[i].referer).toUtf8());
        req.setRawHeader("User-Agent", "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0 Safari/537.36");
        req.setTransferTimeout(30000);
        QNetworkReply* r = nam.get(req);
        QEventLoop loop;
        connect(r, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        const QByteArray body = r->error() == QNetworkReply::NoError ? r->readAll() : QByteArray();
        r->deleteLater();
        if (body.isEmpty()) { log(QStringLiteral("第 %1 張圖片下載失敗，略過").arg(i + 1)); continue; }
        const QString f = tmp.filePath(QStringLiteral("%1.img").arg(i + 1, 3, 10, QLatin1Char('0')));
        QFile out(f);
        if (out.open(QIODevice::WriteOnly)) { out.write(body); out.close(); files << f; }
        bar_->setValue(85 + 10 * (i + 1) / int(slides.size()));
    }
    if (files.isEmpty()) { log(QStringLiteral("沒有下載到任何圖片。")); return {}; }

    auto probe = [this](const QString& path) {
        QProcess p;
        configureProcess(p);
        p.start(ffmpegDir_ + "/ffprobe.exe", {"-v", "error", "-print_format", "json", "-show_entries", "stream=width,height:format=duration", path});
        p.waitForFinished(30000);
        return QJsonDocument::fromJson(p.readAllStandardOutput()).object();
    };
    const double total = probe(audio).value("format").toObject().value("duration").toString().toDouble();
    const double each = total > 0 ? std::max(total / files.size(), 1.0) : 3.0;
    // 畫布 = 第一張圖的實際尺寸（長寬取偶數，最長邊不超過 4096），不縮小高解析的圖；讀不到尺寸用 1080x1920
    int w = 1080, h = 1920;
    for (const auto& sv : probe(files.first()).value("streams").toArray()) {
        const QJsonObject st = sv.toObject();
        if (st.value("width").toInt() > 0) {
            const double k = std::min(1.0, 4096.0 / std::max(st.value("width").toInt(), st.value("height").toInt()));
            w = std::max(2, int(st.value("width").toInt() * k) / 2 * 2);
            h = std::max(2, int(st.value("height").toInt() * k) / 2 * 2);
            break;
        }
    }
    const QString fit = QStringLiteral("scale=%1:%2:force_original_aspect_ratio=decrease,pad=%1:%2:(ow-iw)/2:(oh-ih)/2:color=black,setsar=1,fps=30,format=yuv420p").arg(w).arg(h);
    QString graph;
    const int n = int(files.size());
    for (int i = 0; i < n; ++i) graph += QStringLiteral("[%1:v]%2[v%1];").arg(i).arg(fit);
    for (int i = 0; i < n; ++i) graph += QStringLiteral("[v%1]").arg(i);
    graph += QStringLiteral("concat=n=%1:v=1:a=0[v]").arg(n);
    QStringList a = {"-y", "-loglevel", "error"};
    for (const QString& f : files) a << "-loop" << "1" << "-t" << QString::number(each, 'f', 3) << "-i" << f;
    const QString out = QStringLiteral("%1/擷取_%2.mp4").arg(outDir_, stamp_);
    a << "-i" << audio << "-filter_complex" << graph << "-map" << "[v]" << "-map" << QStringLiteral("%1:a:0").arg(n)
      << "-c:v" << "libx264" << "-preset" << "fast" << "-crf" << "20" << "-c:a" << "aac" << "-b:a" << "192k";
    if (total > 0) a << "-t" << QString::number(total, 'f', 3); else a << "-shortest";   // -shortest 遇到真實的抖音音樂會把聲音截短
    a << "-movflags" << "+faststart" << out;
    lblStatus_->setText(QStringLiteral("合成影片…"));
    QProcess ff;
    configureProcess(ff);
    ff.start(ffmpegDir_ + "/ffmpeg.exe", a);
    ff.waitForFinished(600000);
    if (ff.exitCode() != 0 || !QFileInfo::exists(out)) {
        log(QStringLiteral("合成圖文影片失敗：") + QString::fromUtf8(ff.readAllStandardError()).trimmed().right(300));
        return {};
    }
    return out;
}

QList<MediaCandidate> CaptureDialog::pickCandidates() const
{
    QList<MediaCandidate> all = candidates_.values();
    std::sort(all.begin(), all.end(), [](const MediaCandidate& a, const MediaCandidate& b) { return a.seq < b.seq; });
    QList<MediaCandidate> slides;
    MediaCandidate music;
    if (slideshowParts(&slides, &music)) return {music};       // 圖文貼文：只下載音樂，圖片在 finish() 取得
    all.removeIf([](const MediaCandidate& c) { return c.kind == "slide"; });
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

void CaptureDialog::startAuto()
{
    if (browserOpen_ || running_ > 0) return;
    openBrowser();
    if (!browserOpen_) return;
    targetDuration_ = -1;
    autoState_ = 1;
    queryInFlight_ = false;
    post_ = QJsonObject();
    postSig_.clear();
    autoClock_.start();
    lblStatus_->setText(QStringLiteral("自動擷取：等待網頁開始播放影片…"));
    autoTimer_.start();
    updateButtons();
}

// 每 0.5 秒：還沒鎖定長度就問瀏覽器有沒有正在播放的影片；鎖定後等影音檔案到齊，再開始下載
void CaptureDialog::autoTick()
{
    if (autoState_ == 1) {
        if (autoClock_.elapsed() > kAutoWaitSeconds * 1000) {
            autoFallback(QStringLiteral("一直沒有偵測到正在播放的影片，多半是需要登入或驗證：請在瀏覽器視窗操作（登入後播放影片），再按「下載擷取到的影片」。"));
            return;
        }
        // 有指定抖音影片 ID 就不等偵測到影音才問：網路偵測常比實際播放晚好幾秒，短影片可能已經播完換下一支
        if (queryInFlight_ || !session_ || (candidates_.isEmpty() && session_->startTargetId().isEmpty())) return;
        queryInFlight_ = true;
        connect(session_, &CaptureSession::playingVideo, this, [this](double d) {
            queryInFlight_ = false;
            if (autoState_ != 1) return;
            // 抖音圖文貼文：頁面說目前這篇有圖片就不再看其他影片（音樂也是 <video>，別篇預載的影片也可能「正在播放」）；圖片清單穩定一陣子就開始
            post_ = session_->lastPost();
            if (!post_.value("slides").toArray().isEmpty()) {
                const QString sig = QString::fromUtf8(QJsonDocument(post_.value("slides").toArray()).toJson(QJsonDocument::Compact));
                if (sig != postSig_) { postSig_ = sig; settleClock_.start(); }
                else if (settleClock_.elapsed() >= kAutoSettleMs && slideshowParts(nullptr, nullptr)) {
                    autoTimer_.stop();
                    autoState_ = 0;
                    autoRun_ = true;
                    beginDownloads();
                }
                return;
            }
            if (!(d > 0 && (session_->lastQueryPlaying() || session_->lastQueryTargetHit()))) return;
            targetDuration_ = d;
            if (session_->lastQueryTargetHit())
                log(QStringLiteral("鎖定網址指定的抖音影片（%1）長度：%2 秒").arg(session_->startTargetId()).arg(d, 0, 'f', 2));
            else
                log(QStringLiteral("鎖定正在播放的影片長度：%1 秒").arg(d, 0, 'f', 2));
            lblStatus_->setText(QStringLiteral("自動擷取：等待影像與聲音都出現…"));
            autoState_ = 2;
            settleCount_ = candidates_.size();
            settleClock_.start();
            autoClock_.start();
        }, Qt::SingleShotConnection);
        session_->queryPlayingVideo(session_->startTargetId());
    } else if (autoState_ == 2) {
        // 鎖定可能早於偵測到任何影音（有指定 ID 時不等候選就先問）：至少要有一個候選，才開始算「穩定」
        if (candidates_.isEmpty()) {
            if (autoClock_.elapsed() > kAutoWaitSeconds * 1000)
                autoFallback(QStringLiteral("已鎖定影片長度，但一直沒有偵測到影音檔案：請在瀏覽器裡播放影片後，再按「下載擷取到的影片」。"));
            settleCount_ = 0;
            return;
        }
        if (candidates_.size() != settleCount_) {   // 有新的影音進來：重新計時，等它穩定
            if (settleCount_ == 0) autoClock_.start();   // 第一個候選才出現：8 秒上限從這時算
            settleCount_ = candidates_.size();
            settleClock_.start();
        }
        if (settleClock_.elapsed() < kAutoSettleMs && autoClock_.elapsed() < 8000) return;
        autoTimer_.stop();
        autoState_ = 0;
        autoRun_ = true;
        beginDownloads();   // 長度已鎖定，不再重問
    }
}

// 下載結果和鎖定的長度比對：相符就關瀏覽器；不符留著檔案交給使用者
void CaptureDialog::autoVerify(const QString& path)
{
    autoRun_ = false;
    double got = -1;
    if (slideMode_) {   // 圖文貼文沒有「正在播放的影片」可比對長度；合成成功就算完成
        if (session_) session_->closeBrowser();
        browserOpen_ = false;
        btnOpen_->setEnabled(true);
        log(QStringLiteral("自動擷取完成，已關閉瀏覽器。"));
        updateButtons();
        return;
    }
    if (!ffmpegDir_.isEmpty()) {
        QProcess p;
        configureProcess(p);
        p.start(ffmpegDir_ + "/ffprobe.exe", {"-v", "error", "-print_format", "json", "-show_entries", "format=duration", path});
        p.waitForFinished(30000);
        bool ok = false;
        const double d = QJsonDocument::fromJson(p.readAllStandardOutput()).object().value("format").toObject().value("duration").toString().toDouble(&ok);
        if (ok) got = d;
    }
    const double tolerance = std::max(0.6, targetDuration_ * 0.05);
    if (got > 0 && std::abs(got - targetDuration_) <= tolerance) {
        log(QStringLiteral("長度比對成功：下載 %1 秒，正在播放 %2 秒").arg(got, 0, 'f', 2).arg(targetDuration_, 0, 'f', 2));
        if (session_) session_->closeBrowser();
        browserOpen_ = false;
        btnOpen_->setEnabled(true);
        log(QStringLiteral("自動擷取完成，已關閉瀏覽器。"));
        updateButtons();
        return;
    }
    log(QStringLiteral("長度比對不符：下載 %1 秒，正在播放 %2 秒").arg(got, 0, 'f', 2).arg(targetDuration_, 0, 'f', 2));
    autoFallback(QStringLiteral("下載到的影片長度和正在播放的不符，檔案先留著：請確認瀏覽器裡播放的是要的影片，再按「下載擷取到的影片」重試。"));
}

// 交給使用者：瀏覽器維持開著，原本的按鈕照常可用
void CaptureDialog::autoFallback(const QString& msg)
{
    autoTimer_.stop();
    autoState_ = 0;
    autoRun_ = false;
    lblStatus_->setText(msg);
    log(msg);
    updateButtons();
}

void CaptureDialog::downloadCaptured()
{
    // 自動擷取還在等時使用者按了下載：先停掉自動流程，避免兩邊各自查詢、重複開始下載。
    // 還沒回來的自動查詢結果會被下面的新查詢取代；它的回呼看到 autoState_ == 0 就直接略過。
    if (autoState_ != 0) {
        autoTimer_.stop();
        autoState_ = 0;
        log(QStringLiteral("改為手動下載，停止自動擷取。"));
    }
    autoRun_ = false;
    targetDuration_ = -1;
    post_ = QJsonObject();
    btnDownload_->setEnabled(false);
    if (session_ && browserOpen_) {
        // 先問瀏覽器：現在在播的是哪一支（長度幾秒）。Facebook 等網站會預先載入下一支，要靠長度把它們分開。
        lblStatus_->setText(QStringLiteral("讀取正在播放的影片…"));
        connect(session_, &CaptureSession::playingVideo, this, [this](double d) {
            targetDuration_ = d;
            post_ = session_->lastPost();
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
    slideMode_ = slideshowParts(nullptr, nullptr);
    queue_ = pickCandidates();
    if (queue_.isEmpty()) return;
    QDir().mkpath(outDir_);
    qDeleteAll(items_);
    items_.clear();
    for (int i = 0; i < queue_.size(); ++i) items_ << new Item;
    nextIndex_ = 0;
    running_ = 0;
    doneCount_ = 0;
    stamp_ = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    cancelled_ = false;
    btnDownload_->setEnabled(false);
    btnStop_->setEnabled(true);
    bar_->setValue(0);
    lblStatus_->setText(QStringLiteral("下載中：同時下載 %1 項（最多 %2 個一起）…").arg(queue_.size()).arg(kParallelDownloads));
    pump();
}

// 補滿同時下載的名額；全部結束（或停止後最後一個結束）才收尾
void CaptureDialog::pump()
{
    while (!cancelled_ && running_ < kParallelDownloads && nextIndex_ < queue_.size()) startItem(nextIndex_++);
    if (running_ == 0 && (cancelled_ || nextIndex_ >= queue_.size())) finish();
}

void CaptureDialog::updateProgress()
{
    int sum = 0;
    for (const Item* it : items_) sum += it->pct;
    if (!items_.isEmpty()) bar_->setValue(sum / int(items_.size()));
}

void CaptureDialog::startItem(int i)
{
    const MediaCandidate c = queue_[i];
    Item* it = items_[i];
    log(QStringLiteral("第 %1/%2 項開始：[%3] %4 %5").arg(i + 1).arg(queue_.size()).arg(c.kind, mb(c.size), c.url.left(110)));
    it->clock.start();
    it->buf.clear();

    QStringList a = {"--ignore-config", "--newline", "--color", "never", "--no-playlist", "-q", "--progress",
                     "--no-simulate", "--retries", "10", "--fragment-retries", "10", "--socket-timeout", "30",
                     "--windows-filenames", "--merge-output-format", "mp4/mkv"};
    a << "-o" << QDir::toNativeSeparators(QStringLiteral("%1/擷取_%2_%3.%(ext)s").arg(outDir_, stamp_).arg(i + 1));
    a << "--print" << "after_move:AVD-FILE|%(filepath)s";
    if (!ffmpegDir_.isEmpty()) a << "--ffmpeg-location" << ffmpegDir_;
    if (!c.referer.isEmpty()) a << "--add-header" << "Referer:" + c.referer;
    // fbcdn 這類 CDN 對 yt-dlp 預設的連線特徵會限速（實測同一個 0.6 MB 檔案：預設 86 秒，偽裝成 Chrome 只要 2.5 秒，
    // curl 0.8 秒），所以先以 Chrome 身分連線；沒裝 curl_cffi 時 yt-dlp 會立刻報錯，那一項改用預設方式重試。
    if (!noImpersonate_) a << "--impersonate" << "chrome";
    a << "--" << c.url;

    if (!it->stall) {
        it->stall = new QTimer(this);
        it->stall->setSingleShot(true);
        connect(it->stall, &QTimer::timeout, this, [this, i, it] {
            if (!it->proc) return;
            log(QStringLiteral("第 %1 項超過 %2 秒沒有任何進度，略過這一項。").arg(i + 1).arg(kStallSeconds));
            killTree(*it->proc);   // finished 訊號會補上下一項
        });
    }
    QProcess* proc = new QProcess(this);
    it->proc = proc;
    ++running_;
    configureProcess(*proc);
    proc->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc, &QProcess::readyReadStandardOutput, this, [this, i, it, proc] {
        it->stall->start(kStallSeconds * 1000);   // 有輸出就重新計時
        it->buf += proc->readAllStandardOutput();
        static const QRegularExpression pct(R"(\[download\]\s+([\d.]+)%)");
        for (const QString& line : takeLinesFrom(it->buf)) {
            if (line.startsWith("AVD-FILE|")) {
                it->file = line.mid(9).trimmed();
                log(QStringLiteral("第 %1 項完成（%2 秒）").arg(i + 1).arg(it->clock.elapsed() / 1000));
            } else if (const auto m = pct.match(line); m.hasMatch()) {
                it->pct = int(m.captured(1).toDouble());
                updateProgress();
            } else if (line.startsWith("ERROR") || line.startsWith("WARNING")) {
                log(QStringLiteral("[第 %1 項] %2").arg(i + 1).arg(line));
            }
        }
    });
    connect(proc, &QProcess::finished, this, [this, i, it, proc](int code, QProcess::ExitStatus) {
        it->stall->stop();
        proc->deleteLater();
        it->proc = nullptr;
        --running_;
        if (code != 0 && !cancelled_ && !noImpersonate_ && !it->retried && it->clock.elapsed() < 15000) {
            // 很快就失敗：多半是這個 yt-dlp 沒有偽裝元件。這一項改用預設方式再試一次，之後的項目也都不帶 --impersonate
            it->retried = true;
            noImpersonate_ = true;
            log(QStringLiteral("以 Chrome 身分連線失敗，改用預設方式重試第 %1 項…").arg(i + 1));
            startItem(i);
            return;
        }
        if (code != 0 && !cancelled_) log(QStringLiteral("第 %1 項下載失敗（結束代碼 %2）").arg(i + 1).arg(code));
        it->pct = 100;
        ++doneCount_;
        if (!cancelled_) {
            updateProgress();
            lblStatus_->setText(QStringLiteral("下載中：已完成 %1/%2 項…").arg(doneCount_).arg(queue_.size()));
        }
        pump();
    });
    connect(proc, &QProcess::errorOccurred, this, [this, it, proc](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        log(QStringLiteral("無法執行 yt-dlp"));
        proc->deleteLater();
        it->proc = nullptr;
        --running_;
        cancelled_ = true;
        for (Item* other : items_)
            if (other->proc) killTree(*other->proc);
        pump();
    });
    proc->start(engine_.program, engine_.args(a));
    it->stall->start(kStallSeconds * 1000);
    updateButtons();
}

void CaptureDialog::finish()
{
    if (cancelled_) {
        autoRun_ = false;
        bar_->setValue(0);
        lblStatus_->setText(QStringLiteral("已停止。未完成的 .part 暫存檔留在儲存位置。"));
        log(lblStatus_->text());
        updateButtons();
        return;
    }
    bar_->setValue(100);
    updateButtons();
    QStringList files_;
    for (const Item* it : items_)
        if (!it->file.isEmpty() && QFileInfo::exists(it->file)) files_ << it->file;
    if (files_.isEmpty()) {
        if (autoRun_) autoFallback(QStringLiteral("沒有下載到任何檔案，原因見下方紀錄。可在瀏覽器裡重新播放後再按「下載擷取到的影片」。"));
        else lblStatus_->setText(QStringLiteral("沒有下載到任何檔案，原因見下方紀錄。"));
        return;
    }

    if (slideMode_) {
        QList<MediaCandidate> slides;
        slideshowParts(&slides, nullptr);
        log(QStringLiteral("抖音圖文貼文：%1 張圖片 + 音樂").arg(slides.size()));
        const QString out = makeSlideshow(slides, files_.first());
        if (out.isEmpty()) {
            if (autoRun_) autoFallback(QStringLiteral("合成圖文影片失敗，音樂檔留在儲存位置，原因見下方紀錄。"));
            else lblStatus_->setText(QStringLiteral("合成圖文影片失敗，音樂檔留在儲存位置，原因見下方紀錄。"));
            return;
        }
        for (const QString& f : files_) QFile::remove(f);
        bar_->setValue(100);
        lblStatus_->setText(QStringLiteral("完成：") + QDir::toNativeSeparators(out));
        log(lblStatus_->text());
        if (autoRun_) autoVerify(out);
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
        if (autoRun_) autoVerify(path);
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
        autoRun_ = false;
        return;
    }
    for (const QString& f : files_) QFile::remove(f);
    report(out);
}

bool CaptureDialog::selfTestLive(const QString& url, const QString& png)
{
    show();
    urlEdit_->setText(url);
    if (qEnvironmentVariableIntValue("AVD_AUTO") > 0) {   // 測自動擷取：等到狀態出現「完成：」或停在手動
        btnAuto_->click();
        for (int i = 0; i < 1200 && (autoState_ != 0 || running_ > 0 || autoRun_); ++i) {
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            QThread::msleep(50);
        }
        grab().save(png);
        QFile f(png + ".txt");
        if (f.open(QIODevice::WriteOnly)) f.write(log_->toPlainText().toUtf8());
        return lblStatus_->text().startsWith(QStringLiteral("完成：")) && !browserOpen_;
    }
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
        for (int i = 0; i < 600 && (running_ > 0 || lblStatus_->text().startsWith(QStringLiteral("下載")) || lblStatus_->text().startsWith(QStringLiteral("讀取"))); ++i) {
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
