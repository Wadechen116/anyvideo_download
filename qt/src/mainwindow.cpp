#include "mainwindow.h"

#include "capture.h"
#include "guard.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGridLayout>
#include <QGroupBox>
#include <QHash>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollBar>
#include <QTextEdit>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

using namespace avd;

namespace {

QComboBox* makeCombo(const QStringList& items, int minChars)
{
    auto* cb = new QComboBox;
    cb->addItems(items);
    cb->setMinimumContentsLength(minChars);
    cb->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    return cb;
}

QStringList labels(const QList<Choice>& choices)
{
    QStringList out;
    for (const auto& c : choices) out << c.label;
    return out;
}

QLabel* hint(const QString& text)
{
    auto* l = new QLabel(text);
    l->setStyleSheet("color:#666666");
    return l;
}

}  // namespace

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent)
{
    setWindowTitle(QString("%1 %2 (Qt)").arg(kAppName, kAppVersion));
    setMinimumSize(820, 640);
    resize(900, 740);

    settings_ = Settings::load();
    ffmpegDir_ = findFfmpegDir();

    buildUi();
    loadSettingsIntoUi();
    updateOptionStates();
    refreshEnvLabel();

    appendLog(QString("%1 %2 已啟動").arg(kAppName, kAppVersion));
    if (ffmpegDir_.isEmpty())
        appendLog("找不到 ffmpeg：影音合併與格式轉換都需要它。安裝方式："
                  "winget install Gyan.FFmpeg（裝完重新開啟本程式）", "warn");
    if (!hasDeno())
        appendLog("提示：未安裝 deno。YouTube 需要 JavaScript 執行環境才能取得完整格式，"
                  "若 YouTube 下載失敗或畫質選項很少，請執行 winget install DenoLand.Deno", "warn");
    detectEngineAsync();
}

// ─── 版面 ───────────────────────────────────────────────────────────────────

void MainWindow::buildUi()
{
    auto* central = new QWidget;
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(10, 10, 10, 10);
    setCentralWidget(central);

    // 網址
    auto* gUrl = new QGroupBox("網址（每行一個，可一次貼多個）");
    auto* vUrl = new QVBoxLayout(gUrl);
    urlEdit_ = new QPlainTextEdit;
    urlEdit_->setLineWrapMode(QPlainTextEdit::NoWrap);
    urlEdit_->setFixedHeight(urlEdit_->fontMetrics().lineSpacing() * 5 + 12);
    urlEdit_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(urlEdit_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        // 內建選單是英文，自己做一份中文的
        QMenu m(this);
        m.addAction("貼上", urlEdit_, &QPlainTextEdit::paste);
        m.addAction("剪下", urlEdit_, &QPlainTextEdit::cut);
        m.addAction("複製", urlEdit_, &QPlainTextEdit::copy);
        m.addSeparator();
        m.addAction("全選", urlEdit_, &QPlainTextEdit::selectAll);
        m.addAction("清除", urlEdit_, &QPlainTextEdit::clear);
        m.exec(urlEdit_->mapToGlobal(pos));
    });
    vUrl->addWidget(urlEdit_);
    auto* urlButtons = new QHBoxLayout;
    auto* btnPaste = new QPushButton("貼上");
    auto* btnClear = new QPushButton("清除");
    btnProbe_ = new QPushButton("解析第一個網址");
    connect(btnPaste, &QPushButton::clicked, this, &MainWindow::pasteUrls);
    connect(btnClear, &QPushButton::clicked, urlEdit_, &QPlainTextEdit::clear);
    connect(btnProbe_, &QPushButton::clicked, this, &MainWindow::probe);
    urlButtons->addWidget(btnPaste);
    urlButtons->addWidget(btnClear);
    urlButtons->addWidget(btnProbe_);
    urlButtons->addStretch();
    vUrl->addLayout(urlButtons);
    root->addWidget(gUrl);

    // 輸出設定
    auto* gOpt = new QGroupBox("輸出設定");
    auto* grid = new QGridLayout(gOpt);
    grid->setColumnStretch(5, 1);

    auto* modeRow = new QHBoxLayout;
    modeRow->addWidget(new QLabel("下載類型："));
    rbVideo_ = new QRadioButton("影片");
    rbAudio_ = new QRadioButton("僅音訊");
    auto* modeGroup = new QButtonGroup(this);
    modeGroup->addButton(rbVideo_);
    modeGroup->addButton(rbAudio_);
    connect(rbVideo_, &QRadioButton::toggled, this, &MainWindow::updateOptionStates);
    modeRow->addWidget(rbVideo_);
    modeRow->addSpacing(12);
    modeRow->addWidget(rbAudio_);
    modeRow->addStretch();
    grid->addLayout(modeRow, 0, 0, 1, 6);

    grid->addWidget(new QLabel("影片格式"), 1, 0);
    cbContainer_ = makeCombo(kVideoContainers, 8);
    connect(cbContainer_, &QComboBox::currentIndexChanged, this, &MainWindow::updateOptionStates);
    grid->addWidget(cbContainer_, 1, 1);
    grid->addWidget(new QLabel("畫質"), 1, 2);
    cbQuality_ = makeCombo(labels(kVideoQualities), 8);
    grid->addWidget(cbQuality_, 1, 3);

    grid->addWidget(new QLabel("取捨"), 2, 0);
    cbPriority_ = makeCombo(labels(kPriorities), 30);
    connect(cbPriority_, &QComboBox::currentIndexChanged, this, &MainWindow::updateOptionStates);
    grid->addWidget(cbPriority_, 2, 1, 1, 4, Qt::AlignLeft);

    grid->addWidget(new QLabel("音訊格式"), 3, 0);
    cbAudioFormat_ = makeCombo(kAudioFormats + QStringList{kAudioOriginalLabel}, 8);
    connect(cbAudioFormat_, &QComboBox::currentIndexChanged, this, &MainWindow::updateOptionStates);
    grid->addWidget(cbAudioFormat_, 3, 1);
    grid->addWidget(new QLabel("位元率 (kbps)"), 3, 2);
    cbBitrate_ = makeCombo(kAudioBitrates, 8);
    grid->addWidget(cbBitrate_, 3, 3);

    auto* checks = new QHBoxLayout;
    chkPlaylist_ = new QCheckBox("下載整個播放清單");
    chkMeta_ = new QCheckBox("寫入標題等資訊");
    chkThumb_ = new QCheckBox("嵌入封面縮圖");
    chkSubs_ = new QCheckBox("嵌入字幕（中文／英文）");
    for (auto* c : {chkPlaylist_, chkMeta_, chkThumb_, chkSubs_}) {
        checks->addWidget(c);
        checks->addSpacing(12);
    }
    checks->addStretch();
    grid->addLayout(checks, 4, 0, 1, 6);

    grid->addWidget(new QLabel("登入狀態"), 5, 0);
    cbBrowser_ = makeCombo(labels(kBrowsers), 8);
    grid->addWidget(cbBrowser_, 5, 1);
    grid->addWidget(hint("需要登入才能看的內容，借用瀏覽器已登入的 cookies"), 5, 2, 1, 4);

    grid->addWidget(new QLabel("cookies.txt"), 6, 0);
    auto* ckRow = new QHBoxLayout;
    cookiesFile_ = new QLineEdit;
    cookiesFile_->setReadOnly(true);
    cookiesFile_->setPlaceholderText("（未選擇）選了檔案就優先使用它，不再讀瀏覽器 cookies");
    btnCookiesPick_ = new QPushButton("選擇…");
    btnCookiesClear_ = new QPushButton("清除");
    btnCookiesAuto_ = new QPushButton("自動偵測");
    connect(btnCookiesAuto_, &QPushButton::clicked, this, &MainWindow::autoDetectCookies);
    connect(btnCookiesPick_, &QPushButton::clicked, this, &MainWindow::chooseCookiesFile);
    connect(btnCookiesClear_, &QPushButton::clicked, this, [this] { cookiesFile_->clear(); });
    ckRow->addWidget(cookiesFile_, 1);
    ckRow->addWidget(btnCookiesPick_);
    ckRow->addWidget(btnCookiesClear_);
    ckRow->addWidget(btnCookiesAuto_);
    grid->addLayout(ckRow, 6, 1, 1, 5);

    grid->addWidget(new QLabel("儲存位置"), 7, 0);
    auto* outRow = new QHBoxLayout;
    outDir_ = new QLineEdit;
    btnBrowse_ = new QPushButton("瀏覽…");
    auto* btnOpen = new QPushButton("開啟資料夾");
    connect(btnBrowse_, &QPushButton::clicked, this, &MainWindow::chooseDir);
    connect(btnOpen, &QPushButton::clicked, this, &MainWindow::openDir);
    outRow->addWidget(outDir_, 1);
    outRow->addWidget(btnBrowse_);
    outRow->addWidget(btnOpen);
    grid->addLayout(outRow, 7, 1, 1, 5);
    root->addWidget(gOpt);

    // 動作
    auto* actions = new QHBoxLayout;
    btnStart_ = new QPushButton("開始下載");
    QFont bold = btnStart_->font();
    bold.setBold(true);
    btnStart_->setFont(bold);
    btnCancel_ = new QPushButton("取消");
    btnCancel_->setEnabled(false);
    btnUpdate_ = new QPushButton("更新下載核心");
    btnCapture_ = new QPushButton("從瀏覽器擷取…");
    btnCapture_->setToolTip("網站解析不了、但瀏覽器能播時：開瀏覽器播放，程式抓出實際的影音網址來下載");
    connect(btnCapture_, &QPushButton::clicked, this, [this] { captureFromBrowser(); });
    connect(btnStart_, &QPushButton::clicked, this, &MainWindow::startDownload);
    connect(btnCancel_, &QPushButton::clicked, this, &MainWindow::cancelDownload);
    connect(btnUpdate_, &QPushButton::clicked, this, &MainWindow::updateCore);
    actions->addWidget(btnStart_);
    actions->addWidget(btnCancel_);
    actions->addWidget(btnCapture_);
    actions->addStretch();
    actions->addWidget(btnUpdate_);
    root->addLayout(actions);

    // 進度
    lblItem_ = new QLabel("待機");
    bar_ = new QProgressBar;
    bar_->setRange(0, 100);
    bar_->setTextVisible(false);
    lblStatus_ = hint("");
    root->addWidget(lblItem_);
    root->addWidget(bar_);
    root->addWidget(lblStatus_);

    // 紀錄
    auto* gLog = new QGroupBox("紀錄");
    auto* vLog = new QVBoxLayout(gLog);
    log_ = new QTextEdit;
    log_->setReadOnly(true);
    vLog->addWidget(log_);
    auto* btnClearLog = new QPushButton("清除紀錄");
    connect(btnClearLog, &QPushButton::clicked, log_, &QTextEdit::clear);
    vLog->addWidget(btnClearLog, 0, Qt::AlignRight);
    root->addWidget(gLog, 1);

    // 狀態列
    lblEnv_ = hint("");
    root->addWidget(lblEnv_);

    // 無障礙名稱：螢幕報讀器與 UI Automation（tests/ui_native.ps1）靠它找到下拉選單
    urlEdit_->setAccessibleName("網址");
    cbContainer_->setAccessibleName("影片格式");
    cbQuality_->setAccessibleName("畫質");
    cbPriority_->setAccessibleName("取捨");
    cbAudioFormat_->setAccessibleName("音訊格式");
    cbBitrate_->setAccessibleName("位元率");
    cbBrowser_->setAccessibleName("登入狀態");
    cookiesFile_->setAccessibleName("cookies.txt");
    btnCookiesPick_->setAccessibleName("選擇 cookies.txt");
    btnCookiesClear_->setAccessibleName("清除 cookies.txt");
    btnCookiesAuto_->setAccessibleName("自動偵測 cookies.txt");
    outDir_->setAccessibleName("儲存位置");
    log_->setAccessibleName("紀錄");
}

// ─── 設定 <-> 介面 ──────────────────────────────────────────────────────────

void MainWindow::loadSettingsIntoUi()
{
    const Settings& s = settings_;
    (s.mode == "audio" ? rbAudio_ : rbVideo_)->setChecked(true);
    cbContainer_->setCurrentText(kVideoContainers.contains(s.container) ? s.container : "mp4");
    cbQuality_->setCurrentText(hasLabel(kVideoQualities, s.quality) ? s.quality : kVideoQualities[0].label);
    cbPriority_->setCurrentText(labelOf(kPriorities, s.priority, kPriorities[0].label));
    cbAudioFormat_->setCurrentText(s.audioFormat == kAudioOriginalValue ? kAudioOriginalLabel
                                   : kAudioFormats.contains(s.audioFormat) ? s.audioFormat : "mp3");
    cbBitrate_->setCurrentText(kAudioBitrates.contains(s.audioBitrate) ? s.audioBitrate : "192");
    chkPlaylist_->setChecked(s.playlist);
    chkMeta_->setChecked(s.embedMeta);
    chkThumb_->setChecked(s.embedThumb);
    chkSubs_->setChecked(s.embedSubs);
    cbBrowser_->setCurrentText(hasLabel(kBrowsers, s.browser) ? s.browser : kBrowsers[0].label);
    cookiesFile_->setText(s.cookiesFile);
    outDir_->setText(s.outDir);
}

Settings MainWindow::collectSettings()
{
    Settings s;
    s.outDir = outDir_->text().trimmed();
    s.mode = rbAudio_->isChecked() ? "audio" : "video";
    s.container = cbContainer_->currentText();
    s.quality = cbQuality_->currentText();
    s.priority = valueOf(kPriorities, cbPriority_->currentText(), "quality");
    s.audioFormat = cbAudioFormat_->currentText() == kAudioOriginalLabel ? kAudioOriginalValue : cbAudioFormat_->currentText();
    s.audioBitrate = cbBitrate_->currentText();
    s.playlist = chkPlaylist_->isChecked();
    s.embedMeta = chkMeta_->isChecked();
    s.embedThumb = chkThumb_->isChecked();
    s.embedSubs = chkSubs_->isChecked();
    s.browser = cbBrowser_->currentText();
    s.cookiesFile = cookiesFile_->text().trimmed();
    settings_ = s;
    return s;
}

void MainWindow::updateOptionStates()
{
    const bool video = rbVideo_->isChecked();
    const bool idle = !busy_;
    const bool original = valueOf(kPriorities, cbPriority_->currentText(), "quality") == "original";   // 不轉檔：容器由來源決定
    const bool audioOriginal = cbAudioFormat_->currentText() == kAudioOriginalLabel;
    cbContainer_->setEnabled(idle && video && !original);
    cbQuality_->setEnabled(idle && video);
    cbPriority_->setEnabled(idle && video);
    cbAudioFormat_->setEnabled(idle && !video);
    cbBitrate_->setEnabled(idle && !video && !audioOriginal && !kLosslessAudio.contains(cbAudioFormat_->currentText()));

    const QString target = video ? (original ? QString() : cbContainer_->currentText()) : cbAudioFormat_->currentText();
    chkThumb_->setEnabled(idle && kThumbOk.contains(target));      // 容器還不知道時（原始品質、原始音訊）不嵌封面
    chkSubs_->setEnabled(idle && video && (original || kSubsOk.contains(cbContainer_->currentText())));
    for (QWidget* w : {static_cast<QWidget*>(rbVideo_), static_cast<QWidget*>(rbAudio_),
                       static_cast<QWidget*>(chkPlaylist_), static_cast<QWidget*>(chkMeta_),
                       static_cast<QWidget*>(cbBrowser_), static_cast<QWidget*>(outDir_),
                       static_cast<QWidget*>(btnBrowse_), static_cast<QWidget*>(btnCookiesPick_),
                       static_cast<QWidget*>(btnCookiesClear_), static_cast<QWidget*>(btnCookiesAuto_)})
        w->setEnabled(idle);
    urlEdit_->setReadOnly(busy_);

    const bool ready = idle && engine_.ok();
    btnStart_->setEnabled(ready);
    btnProbe_->setEnabled(ready && !probing_);
    btnUpdate_->setEnabled(idle && engineKnown_ && !updating_);
    btnCancel_->setEnabled(busy_);
    btnCapture_->setEnabled(ready);
}

void MainWindow::captureFromBrowser(const QString& startUrl)
{
    const Settings s = collectSettings();
    if (s.outDir.isEmpty()) {
        QMessageBox::warning(this, kAppName, "請選擇儲存位置。");
        return;
    }
    const QStringList list = urls();
    CaptureDialog dlg(engine_, ffmpegDir_, s.outDir, startUrl.isEmpty() ? list.value(0) : startUrl, this);
    dlg.exec();
}

void MainWindow::refreshEnvLabel()
{
    const QString ytv = !engineKnown_ ? QStringLiteral("偵測中…") : engine_.ok() ? engine_.version : QStringLiteral("未安裝");
    const QString ff = ffmpegDir_.isEmpty() ? QStringLiteral("未找到") : QStringLiteral("已找到");
    lblEnv_->setText(QStringLiteral("yt-dlp %1　｜　ffmpeg %2　｜　Qt %3").arg(ytv, ff, qVersion()));
}

// ─── 下載核心偵測 ───────────────────────────────────────────────────────────

void MainWindow::detectEngineAsync()
{
    const bool firstTime = !engineKnown_;
    engineKnown_ = false;
    updateOptionStates();
    refreshEnvLabel();
    // 偵測要實際執行 yt-dlp --version，放到背景執行緒，視窗才不會卡住
    QThread* t = QThread::create([this, firstTime] {
        Engine e = detectEngine();
        QMetaObject::invokeMethod(this, [this, e, firstTime] { onEngineDetected(e, firstTime); },
                                  Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void MainWindow::onEngineDetected(const Engine& engine, bool firstTime)
{
    engine_ = engine;
    engineKnown_ = true;
    refreshEnvLabel();
    updateOptionStates();
    if (engine_.ok()) {
        if (!firstTime) appendLog(QString("下載核心 yt-dlp %1 可以使用。").arg(engine_.version), "ok");
        if (const int age = coreAgeDays(engine_.version); age > 30)
            appendLog(QString("提示：下載核心已 %1 天沒更新。網站常改版，下載失敗時請先按「更新下載核心」。").arg(age), "warn");
        return;
    }
    appendLog("找不到下載核心 yt-dlp。", "error");
    if (firstTime && QMessageBox::question(this, kAppName, "尚未安裝下載核心 yt-dlp。\n\n要現在自動安裝嗎？") ==
                         QMessageBox::Yes)
        updateCore();
}

// ─── 動作 ───────────────────────────────────────────────────────────────────

QStringList MainWindow::urls() const
{
    // 貼上的常常是整段分享文字（抖音、小紅書的「分享」會連標題與表情符號一起複製），從裡面找出網址；
    // 一行裡沒有網址的（例如 yt-dlp 認得的影片代號）原樣保留。
    QStringList out;
    for (const QString& line : urlEdit_->toPlainText().split('\n')) {
        const QString t = line.trimmed();
        if (t.isEmpty() || t.startsWith('#')) continue;
        const QStringList found = extractUrls(t);
        if (found.isEmpty()) { out << t; continue; }
        for (const QString& u : found)
            if (const QString n = normalizeUrl(u); !out.contains(n)) out << n;
    }
    return out;
}

void MainWindow::pasteUrls()
{
    const QString text = QApplication::clipboard()->text().trimmed();
    if (text.isEmpty()) return;
    QString existing = urlEdit_->toPlainText();
    if (!existing.trimmed().isEmpty() && !existing.endsWith('\n')) existing += '\n';
    urlEdit_->setPlainText(existing + text + '\n');
}

void MainWindow::chooseDir()
{
    const QString start = outDir_->text().isEmpty() ? QDir::homePath() : outDir_->text();
    const QString d = QFileDialog::getExistingDirectory(this, "選擇儲存位置", start);
    if (!d.isEmpty()) outDir_->setText(QDir::toNativeSeparators(d));
}

void MainWindow::chooseCookiesFile()
{
    const QString cur = cookiesFile_->text().trimmed();
    const QString start = cur.isEmpty() ? QDir::homePath() : cur;
    const QString f = QFileDialog::getOpenFileName(this, "選擇 cookies.txt", start, "cookies (*.txt);;所有檔案 (*)");
    if (!f.isEmpty()) cookiesFile_->setText(QDir::toNativeSeparators(f));
}

void MainWindow::autoDetectCookies()
{
    const QStringList found = avd::detectCookieFiles(avd::defaultCookieDirs(outDir_->text()));
    if (found.isEmpty()) {
        QMessageBox::information(this, "自動偵測",
            QStringLiteral("找不到 cookies.txt。\n\n請先用瀏覽器擴充功能（例如 Get cookies.txt LOCALLY）在 douyin.com 匯出，"
                           "檔案放在「下載」資料夾，再按一次自動偵測。"));
        return;
    }
    const QString best = found.first();
    cookiesFile_->setText(best);
    QMessageBox::information(this, "自動偵測",
        QStringLiteral("已選用：%1\n修改時間：%2\n含抖音/TikTok cookies：%3\n\n共找到 %4 個可用的檔案。")
            .arg(best, QFileInfo(best).lastModified().toString("yyyy-MM-dd HH:mm"),
                 avd::cookieFileHasDouyin(best) ? QStringLiteral("是") : QStringLiteral("否"))
            .arg(found.size()));
}

void MainWindow::openDir()
{
    const QString d = outDir_->text().trimmed();
    if (!QDir(d).exists()) {
        QMessageBox::information(this, kAppName, "資料夾還不存在，第一次下載時會自動建立。");
        return;
    }
    QDesktopServices::openUrl(QUrl::fromLocalFile(d));
}

void MainWindow::probe()
{
    const QStringList list = urls();
    if (list.isEmpty()) {
        QMessageBox::information(this, kAppName, "請先貼上網址。");
        return;
    }
    appendLog("解析中：" + list[0]);
    probing_ = true;
    updateOptionStates();
    auto* job = new ProbeJob(list[0], collectSettings(), engine_, this);
    connect(job, &ProbeJob::log, this, &MainWindow::appendLog);
    connect(job, &ProbeJob::done, this, [this, job](bool ok, const QString& text) {
        probing_ = false;
        updateOptionStates();
        appendLog(text, ok ? "probe" : "error");
        job->deleteLater();
    });
    job->start();
}

void MainWindow::startDownload()
{
    const QStringList list = urls();
    if (list.isEmpty()) {
        QMessageBox::information(this, kAppName, "請先貼上至少一個網址。");
        return;
    }
    Settings s = collectSettings();
    if (s.outDir.isEmpty()) {
        QMessageBox::warning(this, kAppName, "請選擇儲存位置。");
        return;
    }
    if (ffmpegDir_.isEmpty()) {
        QMessageBox::warning(this, kAppName,
                             "找不到 ffmpeg，無法合併影音或轉換格式。\n\n"
                             "請先安裝：winget install Gyan.FFmpeg\n裝完後重新開啟本程式。");
        return;
    }
    if (!QDir().mkpath(s.outDir)) {
        QMessageBox::critical(this, kAppName, "無法建立儲存資料夾：\n" + s.outDir);
        return;
    }
    if (!guardBeforeDownload(s)) return;
    s.save();

    setBusy(true);
    const QString prio = s.priority == "compat" ? "相容優先" : s.priority == "original" ? "原始品質（不轉檔）" : "畫質優先";
    const QString what = s.mode == "video"
        ? (s.priority == "original" ? QString("影片 %1 / %2").arg(s.quality, prio) : QString("影片 %1 / %2 / %3").arg(s.container, s.quality, prio))
        : s.audioFormat == kAudioOriginalValue ? QString("音訊 原始格式（不轉檔）")
        : "音訊 " + s.audioFormat + (kLosslessAudio.contains(s.audioFormat) ? "" : " / " + s.audioBitrate + " kbps");
    appendLog(QString("開始下載 %1 個網址（%2）→ %3").arg(list.size()).arg(what, s.outDir));

    job_ = new DownloadJob(list, s, engine_, ffmpegDir_, this);
    connect(job_, &DownloadJob::log, this, &MainWindow::appendLog);
    connect(job_, &DownloadJob::item, this, [this](int i, int n, const QString& url) {
        lblItem_->setText(QString("第 %1/%2 個：%3").arg(i).arg(n).arg(url));
        setBar(0);
        lblStatus_->setText("解析網址中…");
    });
    connect(job_, &DownloadJob::progress, this, &MainWindow::onProgress);
    connect(job_, &DownloadJob::status, this, &MainWindow::onStatus);
    connect(job_, &DownloadJob::done, this, &MainWindow::onDone);
    job_->start();
}

// 下載前的設定防呆（見 guard.h）。s 可能被改成修正後的設定；使用者取消或遇到致命問題回傳 false。
bool MainWindow::guardBeforeDownload(Settings& s)
{
    const QList<GuardIssue> issues = checkDownloadSettings(s, probeOutDir(s.outDir));
    QStringList fatal;
    for (const GuardIssue& i : issues) if (i.fatal) fatal << describeGuardIssue(i);
    if (!fatal.isEmpty()) {
        QMessageBox::critical(this, kAppName, fatal.join("\n\n"));
        return false;
    }
    if (issues.isEmpty()) return true;
    QStringList lines;
    bool anyFix = false;
    for (const GuardIssue& i : issues) { lines << "• " + describeGuardIssue(i); anyFix |= !i.fix.isEmpty(); }
    if (!anyFix)
        return QMessageBox::question(this, kAppName, lines.join("\n\n") + "\n\n要照原設定開始嗎？", QMessageBox::Ok | QMessageBox::Cancel) == QMessageBox::Ok;
    QMessageBox box(QMessageBox::Question, kAppName, lines.join("\n\n") + "\n\n要自動修正嗎？（修正後保留最高畫質）",
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::Cancel, this);
    box.button(QMessageBox::Yes)->setText("自動修正並開始");
    box.button(QMessageBox::No)->setText("照原設定開始");
    box.button(QMessageBox::Cancel)->setText("取消");
    box.setDefaultButton(QMessageBox::Yes);
    const int answer = box.exec();
    if (answer == QMessageBox::Cancel) return false;
    if (answer == QMessageBox::Yes) {
        s = applyGuardFixes(s, issues);
        settings_ = s;
        loadSettingsIntoUi();
        updateOptionStates();
    }
    return true;
}

void MainWindow::cancelDownload()
{
    if (!job_) return;
    btnCancel_->setEnabled(false);
    lblStatus_->setText("取消中…");
    job_->cancel();
}

void MainWindow::updateCore()
{
    appendLog("更新下載核心 yt-dlp …");
    updating_ = true;
    updateOptionStates();
    auto* job = new UpdateJob(engine_, this);
    connect(job, &UpdateJob::log, this, &MainWindow::appendLog);
    connect(job, &UpdateJob::done, this, [this, job](int rc) {
        updating_ = false;
        job->deleteLater();
        if (rc == 0) {
            appendLog("下載核心已更新，重新偵測中…", "ok");
            detectEngineAsync();  // 不必重新開啟程式：下次呼叫 yt-dlp 就是新版本
        } else {
            appendLog(QString("更新失敗（結束代碼 %1），請看上方訊息。").arg(rc), "error");
            updateOptionStates();
        }
    });
    job->start();
}

void MainWindow::setBusy(bool busy)
{
    busy_ = busy;
    updateOptionStates();
    if (!busy) setBar(0);
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    if (busy_ && QMessageBox::question(this, kAppName, "還在下載中，確定要結束嗎？") != QMessageBox::Yes) {
        e->ignore();
        return;
    }
    if (job_) {
        disconnect(job_, nullptr, this, nullptr);
        job_->cancel();
    }
    collectSettings().save();
    e->accept();
}

// ─── 下載回報 ───────────────────────────────────────────────────────────────

void MainWindow::onProgress(double pct, const QString& text)
{
    setBar(pct);
    lblStatus_->setText(text);
}

void MainWindow::onStatus(const QString& text)
{
    setBar(-1);  // 後處理沒有百分比，改用來回跑的進度條
    lblStatus_->setText(text);
}

void MainWindow::onDone(int ok, int fail, bool cancelled)
{
    const QStringList retryByCapture = job_ ? job_->captureCandidates() : QStringList();
    if (job_) job_->deleteLater();
    setBusy(false);
    setBar(!cancelled && fail == 0 ? 100 : 0);
    QString msg;
    if (cancelled) {
        msg = QString("已取消。完成 %1 個。未完成的 .part 暫存檔保留在資料夾，下次同網址可接續下載。").arg(ok);
        appendLog(msg, "warn");
    } else {
        msg = QString("全部結束：成功 %1 個").arg(ok) +
              (fail ? QString("，失敗 %1 個（原因見紀錄）").arg(fail) : QString());
        appendLog(msg, fail == 0 ? "ok" : "warn");
    }
    lblItem_->setText("待機");
    lblStatus_->setText(msg);
    if (!cancelled && fail == 0 && retryByCapture.isEmpty()) {
        // 全部成功：清空網址區與紀錄，準備貼下一批（有失敗或取消時保留，才看得到原因、也能重試）
        urlEdit_->clear();
        log_->clear();
    }

    // 抖音、小紅書、TikTok 的解析器常因網站改版或要求登入而失敗，但瀏覽器能播，就能用「從瀏覽器擷取」抓下來
    if (!cancelled && !retryByCapture.isEmpty() && engine_.ok() &&
        QMessageBox::question(this, kAppName,
            QString("下面的網址下載失敗：\n\n%1\n\n這類網站常常可以改用「從瀏覽器擷取」：瀏覽器能播，就能抓。要現在開啟嗎？").arg(retryByCapture.join('\n'))) ==
            QMessageBox::Yes)
        captureFromBrowser(retryByCapture.first());
}

void MainWindow::setBar(double pct)
{
    if (pct < 0) {
        bar_->setRange(0, 0);
        return;
    }
    bar_->setRange(0, 100);
    bar_->setValue(static_cast<int>(qBound(0.0, pct, 100.0)));
}

void MainWindow::appendLog(const QString& text, const QString& tag)
{
    // 一般訊息用介面文字色；其他顏色在深色背景上換成較亮的版本，才看得清楚
    static const QHash<QString, QPair<QColor, QColor>> colors = {  // {淺色背景, 深色背景}
        {"ok", {QColor("#1a7f37"), QColor("#56d364")}},
        {"warn", {QColor("#b35900"), QColor("#f0a040")}},
        {"error", {QColor("#c62828"), QColor("#ff7b72")}},
        {"probe", {QColor("#1f4e8c"), QColor("#79b8ff")}},
    };
    const bool dark = log_->palette().color(QPalette::Base).lightness() < 128;
    QTextCharFormat fmt;
    if (colors.contains(tag))
        fmt.setForeground(dark ? colors[tag].second : colors[tag].first);
    else
        fmt.setForeground(log_->palette().color(QPalette::Text));
    QTextCursor cur(log_->document());
    cur.movePosition(QTextCursor::End);
    const QString stamp = QDateTime::currentDateTime().toString("HH:mm:ss");
    const QStringList lines = text.split('\n');
    for (int i = 0; i < lines.size(); ++i) {
        const QString prefix = i == 0 ? "[" + stamp + "] " : QString(11, ' ');
        cur.insertText(prefix + lines[i] + "\n", fmt);
    }
    log_->verticalScrollBar()->setValue(log_->verticalScrollBar()->maximum());
}
