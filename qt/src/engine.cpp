#include "engine.h"

#include <QCoreApplication>
#include <QDate>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTimer>

#include <algorithm>
#include <memory>
#include <set>

#ifdef Q_OS_WIN
#define NOMINMAX
#include <windows.h>
#endif

namespace avd {

namespace {

// yt-dlp 的進度改用這兩個樣板輸出，一行一筆，欄位用 | 分隔；沒有值的欄位會印成 NA。
// 以 AVD- 開頭，和 yt-dlp 自己的訊息分得開。
const QString kDownloadTemplate = QStringLiteral(
    "download:AVD-DL|%(progress.status)s|%(progress.downloaded_bytes)s|%(progress.total_bytes)s"
    "|%(progress.total_bytes_estimate)s|%(progress.speed)s|%(progress.eta)s"
    "|%(progress.fragment_index)s|%(progress.fragment_count)s"
    "|%(info.vcodec)s|%(info.acodec)s|%(info.playlist_index)s|%(info.n_entries)s");
const QString kPostprocessTemplate =
    QStringLiteral("postprocess:AVD-PP|%(progress.status)s|%(progress.postprocessor)s");

const QStringList kPipInstall = {"-m", "pip", "install", "-U", "yt-dlp[default,curl-cffi]"};

double num(const QString& s)  // "NA" 或解析不了 → -1
{
    bool ok = false;
    const double v = s.toDouble(&ok);
    return ok ? v : -1;
}

QString versionOf(const QString& program, const QStringList& args)
{
    QProcess p;
    configureProcess(p);
    p.start(program, args);
    if (!p.waitForStarted(5000)) return {};
    if (!p.waitForFinished(60000)) {
        p.kill();
        p.waitForFinished(2000);
        return {};
    }
    if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) return {};
    const QString v = QString::fromUtf8(p.readAllStandardOutput()).trimmed().section('\n', 0, 0).trimmed();
    static const QRegularExpression looksLikeVersion(R"(^\d{4}\.\d)");
    return looksLikeVersion.match(v).hasMatch() ? v : QString();
}

// 把輸出位元組切成行，保留最後一段不完整的行等下次
QStringList takeLines(QByteArray& buffer)
{
    QStringList lines;
    int nl;
    while ((nl = buffer.indexOf('\n')) >= 0) {
        QString line = QString::fromUtf8(buffer.left(nl));
        buffer.remove(0, nl + 1);
        if (line.endsWith('\r')) line.chop(1);
        lines << line;
    }
    return lines;
}

}  // namespace

// ─── 環境偵測 ───────────────────────────────────────────────────────────────

Engine detectEngine()
{
    QStringList exes;
    const QString local = QDir(QCoreApplication::applicationDirPath()).filePath("yt-dlp.exe");
    if (QFileInfo::exists(local)) exes << QDir::toNativeSeparators(local);
    const QString onPath = QStandardPaths::findExecutable("yt-dlp");
    if (!onPath.isEmpty()) exes << QDir::toNativeSeparators(onPath);

    for (const QString& exe : exes) {
        const QString v = versionOf(exe, {"--version"});
        if (!v.isEmpty()) return {Engine::Kind::Exe, exe, {}, v};
    }
    const QString py = QStandardPaths::findExecutable("py");
    if (!py.isEmpty()) {
        const QStringList prefix = {"-3", "-m", "yt_dlp"};
        const QString v = versionOf(py, prefix + QStringList{"--version"});
        if (!v.isEmpty()) return {Engine::Kind::PyModule, QDir::toNativeSeparators(py), prefix, v};
    }
    return {};
}

QString findFfmpegDir()
{
    const QString exe = QStandardPaths::findExecutable("ffmpeg");
    return exe.isEmpty() ? QString() : QDir::toNativeSeparators(QFileInfo(exe).absolutePath());
}

bool hasDeno() { return !QStandardPaths::findExecutable("deno").isEmpty(); }

QString pluginDir()
{
    return QDir::toNativeSeparators(QDir(QCoreApplication::applicationDirPath()).filePath("yt-dlp-plugins"));
}

QString pluginDirIfPresent()
{
    const QString dir = pluginDir();
    const bool present =
        QFileInfo::exists(QDir(dir).filePath("anyvideo/yt_dlp_plugins/postprocessor/ensure_h264.py"));
    return present ? dir : QString();
}

bool needsEnsureH264(const Settings& s)
{
    return s.mode == "video" && s.priority != "original" &&
           (s.container == "mov" || (s.priority == "compat" && kCompatH264Containers.contains(s.container)));
}

// ─── 命令列參數 ─────────────────────────────────────────────────────────────

QStringList buildDownloadArgs(const Settings& s, const QString& ffmpegDir, const QString& plugins,
                              const QString& url, const RetryTweaks& tweaks)
{
    QStringList a;
    // --ignore-config：不讓使用者自己的 yt-dlp 設定檔改變結果
    a << "--ignore-config" << "--newline" << "--color" << "never";
    a << (s.playlist ? "--yes-playlist" : "--no-playlist");
    // 命令列預設會把 multi_video 清單合併成一個檔；Python 版（函式庫）不會，照 Python 版
    a << "--concat-playlist" << "never";

    // .150B 是以位元組截斷標題，避免超過 Windows 的路徑長度限制
    const QString tmpl = s.playlist
        ? QStringLiteral("%(playlist_title,playlist|playlist).80B\\%(playlist_index|0)03d - %(title).150B [%(id)s].%(ext)s")
        : QStringLiteral("%(title).150B [%(id)s].%(ext)s");
    QString out = s.outDir;
    out.replace('/', '\\');
    while (out.endsWith('\\')) out.chop(1);
    a << "-o" << out + "\\" + tmpl;

    a << "--windows-filenames" << "--retries" << "10" << "--fragment-retries" << "10"
      << "--extractor-retries" << "5" << "--socket-timeout" << "30"
      << "--retry-sleep" << "http:exp=1:20" << "--retry-sleep" << "fragment:exp=1:20"
      << "--concurrent-fragments" << "4";
    if (!ffmpegDir.isEmpty()) a << "--ffmpeg-location" << ffmpegDir;
    const QString browser = valueOf(kBrowsers, s.browser);
    if (!browser.isEmpty()) a << "--cookies-from-browser" << browser;
    if (!plugins.isEmpty()) a << "--plugin-dirs" << plugins;
    if (tweaks.impersonate) a << "--impersonate" << "chrome";
    a << "--progress-template" << kDownloadTemplate << "--progress-template" << kPostprocessTemplate;

    QString target;
    if (s.mode == "video") {
        const QString c = s.container;
        const QString h = valueOf(kVideoQualities, s.quality);
        const QString hf = h.isEmpty() ? QString() : "[height<=" + h + "]";
        a << "-f" << (tweaks.relaxFormat ? QStringLiteral("bv*+ba/b") : "bestvideo*" + hf + "+bestaudio/best" + hf + "/best");

        if (s.priority == "original") {
            // 原始品質：不指定排序（yt-dlp 自己的順序就是畫質優先：解析度、幀率、HDR、編碼、位元率，聲音挑位元率最高）、
            // 不轉檔、不重新封裝成別的容器、不碰 EnsureH264。合併只是把影像與聲音串流複製進同一個容器：
            // 兩邊都放得進 mp4 就用 mp4，否則 mkv（什麼都裝得下）。結果與網站提供的串流解碼後逐格相同。
            a << "--merge-output-format" << "mp4/mkv";
            if (s.embedSubs) a << "--embed-subs" << "--sub-langs" << "zh.*,en.*";
            if (s.embedMeta) a << "--embed-metadata" << "--embed-chapters";
            a << "--" << url;
            return a;
        }

        // 畫質優先先比解析度；相容優先先比編碼（vcodec:h264 把 H.264 排第一）。
        // 詳細說明見 py/anyvideo_download.py 的 build_ydl_options()。
        const bool compat = s.priority == "compat";
        QString sort;
        if (compat)
            sort = c == "webm" ? "vcodec:vp9,acodec:opus,res,ext:webm:webm" : "vcodec:h264,acodec:aac,res,ext:mp4:m4a";
        else if (c == "webm")
            sort = "res,ext:webm:webm";
        else if (c == "mkv")
            sort = "res";
        else if (c == "mov")  // mov 裝不下 AV1／VP9，同解析度時挑 H.264 省掉重新編碼
            sort = "res,vcodec:h264,acodec:aac,ext:mp4:m4a";
        else
            sort = "res,ext:mp4:m4a";
        if (!tweaks.relaxFormat) a << "-S" << sort;
        a << "--merge-output-format" << (c == "mkv" ? "mkv" : c == "webm" ? "webm/mkv" : "mp4/mkv");

        // yt-dlp 命令列的後處理順序固定是：--use-postprocessor → 重新封裝 → 轉檔 →
        // 字幕 → 資訊 → 封面。EnsureH264 正好排在最前面。
        if (c == "mov" || (compat && kCompatH264Containers.contains(c))) {
            a << "--use-postprocessor" << QString("EnsureH264:video_ok=") + (compat ? "h264" : "h264,hevc");
            a << "--remux-video" << c;
        } else if (c == "mkv") {
            a << "--remux-video" << "mkv";
        } else {
            a << "--recode-video" << c;
        }
        if (s.embedSubs && kSubsOk.contains(c)) a << "--embed-subs" << "--sub-langs" << "zh.*,en.*";
        target = c;
    } else {
        a << "-f" << "bestaudio/best" << "-x" << "--audio-format"
          << (s.audioFormat == "ogg" ? QStringLiteral("vorbis") : s.audioFormat);
        // 「原始（不轉檔）」= --audio-format best：聲音串流原封不動存成它原本的格式，沒有重新編碼
        if (!kLosslessAudio.contains(s.audioFormat) && s.audioFormat != kAudioOriginalValue) a << "--audio-quality" << s.audioBitrate;
        target = s.audioFormat;
    }

    if (s.embedMeta) a << "--embed-metadata" << "--embed-chapters";
    if (s.embedThumb && kThumbOk.contains(target)) a << "--embed-thumbnail";
    a << "--" << url;
    return a;
}

QStringList buildProbeArgs(const Settings& s, const QString& url)
{
    QStringList a{"--ignore-config", "--color", "never", "-J", "--flat-playlist"};
    a << (s.playlist ? "--yes-playlist" : "--no-playlist");
    const QString browser = valueOf(kBrowsers, s.browser);
    if (!browser.isEmpty()) a << "--cookies-from-browser" << browser;
    a << "--" << url;
    return a;
}

// ─── 失敗原因判斷 ───────────────────────────────────────────────────────────

QString classifyError(const QString& text)
{
    // 順序重要：先比對的優先。與 py/anyvideo_download.py 的 _ERROR_KINDS 相同。
    static const QList<QPair<QString, QRegularExpression>> kinds = [] {
        const auto re = [](const char* p) { return QRegularExpression(QString::fromUtf8(p)); };
        return QList<QPair<QString, QRegularExpression>>{
            {"drm", re("DRM")},
            {"disk", re("No space left|Permission denied|File name too long|path too long|Errno 28|Errno 13")},
            {"bot", re("confirm you.re not a bot|Sign in to confirm")},
            {"login", re("[Ll]og(?:ged)?[- ]?in|[Ss]ign in|private video|members-only|[Cc]ookies|"
                         "requires authentication|HTTP Error 401")},
            {"geo", re("geo[- ]?restrict|not available in your country|in your location")},
            {"impersonate", re("impersonat|HTTP Error 403|HTTP Error 429|Cloudflare|Just a moment|\\b403\\b")},
            // 憑證驗證失敗不是網路不穩：重試一百次結果都一樣，所以放在 network 之前，單獨一類、不重試
            {"cert", re("CERTIFICATE_VERIFY_FAILED|certificate verify failed|self[- ]signed certificate|unable to get local issuer")},
            {"format", re("[Rr]equested format is not available|No video formats found")},
            {"network", re("timed? ?out|Temporary failure|Connection (?:reset|aborted|refused)|IncompleteRead|"
                           "RemoteDisconnected|Name or service not known|getaddrinfo|SSL|HTTP Error 5\\d\\d|"
                           "Unable to download (?:webpage|JSON)|EOF occurred")},
            {"ffmpeg", re("ffmpeg|ffprobe|Postprocessing")},
            {"parse", re("Cannot parse data")},
            {"update", re("Unsupported URL|Unable to extract|nsig|signature|player response|"
                          "Failed to parse JSON|Unable to download.*(?:API|player)|HTTP Error 404")},
        };
    }();
    for (const auto& k : kinds)
        if (k.second.match(text).hasMatch()) return k.first;
    return "unknown";
}

int coreAgeDays(const QString& version)
{
    const QStringList p = version.split('.');
    if (p.size() < 3) return -1;
    const QDate d(p[0].toInt(), p[1].toInt(), p[2].section(QRegularExpression("\\D"), 0, 0).toInt());
    return d.isValid() ? int(d.daysTo(QDate::currentDate())) : -1;
}

QString errorHint(const QString& kind, const Settings& s)
{
    if (kind == "drm") return QStringLiteral("這個內容有 DRM 保護，無法下載。");
    if (kind == "disk") return QStringLiteral("寫入失敗：請確認儲存位置還有空間、有寫入權限，且路徑不要太深。");
    if (kind == "bot")
        return QStringLiteral("網站要求證明不是機器人。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。");
    if (kind == "login")
        return s.browser == QStringLiteral("不使用")
                   ? QStringLiteral("這個內容需要登入。「登入狀態」選一個已登入該網站的瀏覽器（Firefox 最可靠）再試。")
                   : QStringLiteral("已使用瀏覽器 cookies 仍要求登入：請確認有在該瀏覽器登入，"
                                    "並先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies）。");
    if (kind == "geo") return QStringLiteral("網站限制地區，需要該地區的網路（VPN／代理）才能下載。");
    if (kind == "impersonate")
        return QStringLiteral("網站擋掉了程式的連線。若尚未安裝瀏覽器偽裝元件 curl_cffi，"
                              "按「更新下載核心」安裝後再試。");
    if (kind == "cert")
        return QStringLiteral("HTTPS 憑證驗證失敗：這台電腦連到這個網站時，拿到的憑證不是網站本身的（網路供應商、公司防火牆、防毒軟體的 HTTPS 掃描，"
                              "或網站被導向到別的伺服器）。重試沒有用，請檢查網路環境；不要為了下載而關閉憑證檢查。");
    if (kind == "network")
        return QStringLiteral("網路連線不穩或被中斷，已自動重試仍失敗。請檢查網路／VPN 後再試，續傳會接著下載。");
    if (kind == "ffmpeg")
        return QStringLiteral("ffmpeg 處理失敗：確認 ffmpeg 可用（winget install Gyan.FFmpeg），或換一種輸出格式。");
    if (kind == "parse")
        return QStringLiteral("網站回傳的頁面解析不了。最常見是私人／朋友限定／社團內容（Facebook、Instagram 等）："
                              "「登入狀態」選一個已登入該網站的瀏覽器再試；公開內容仍失敗才是網站改版，請更新下載核心。");
    if (kind == "update") return QStringLiteral("網站可能改版了：按「更新下載核心」再試。");
    return {};
}

bool planRetry(const QString& kind, QSet<QString>& tried, RetryTweaks& tweaks, QString& message)
{
    if (tried.contains(kind)) return false;
    if (kind == "network") {
        message = QStringLiteral("網路不穩，稍後重試…");
    } else if (kind == "format") {
        tweaks.relaxFormat = true;
        message = QStringLiteral("找不到符合的格式，改用寬鬆的格式選擇重試…");
    } else if (kind == "impersonate") {
        tweaks.impersonate = true;
        message = QStringLiteral("改以 Chrome 瀏覽器身分連線重試…");
    } else {
        return false;
    }
    tried.insert(kind);
    return true;
}

QString fmtBytes(double n)
{
    if (n <= 0) return "?";
    for (const char* unit : {"B", "KB", "MB", "GB"}) {
        if (n < 1024) return QString::number(n, 'f', 1) + " " + unit;
        n /= 1024;
    }
    return QString::number(n, 'f', 1) + " TB";
}

QString fmtEta(double sec)
{
    if (sec < 0) return "?";
    const qint64 t = static_cast<qint64>(sec);
    if (t >= 3600)
        return QString("%1:%2:%3").arg(t / 3600).arg(t / 60 % 60, 2, 10, QChar('0')).arg(t % 60, 2, 10, QChar('0'));
    return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0'));
}

void configureProcess(QProcess& p)
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONUTF8", "1");
    p.setProcessEnvironment(env);
#ifdef Q_OS_WIN
    p.setCreateProcessArgumentsModifier(
        [](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
#endif
}

void killTree(QProcess& p)
{
    if (p.state() == QProcess::NotRunning) return;
#ifdef Q_OS_WIN
    QProcess tk;
    configureProcess(tk);
    tk.start("taskkill", {"/PID", QString::number(p.processId()), "/T", "/F"});
    tk.waitForFinished(5000);
#endif
    if (p.state() != QProcess::NotRunning) p.kill();
}

// ─── 下載 ───────────────────────────────────────────────────────────────────

DownloadJob::DownloadJob(QStringList urls, Settings s, Engine engine, QString ffmpegDir, QObject* parent)
    : QObject(parent), urls_(std::move(urls)), settings_(std::move(s)), engine_(std::move(engine)),
      ffmpegDir_(std::move(ffmpegDir))
{
}

void DownloadJob::start()
{
    // 需要外掛卻找不到時，在開始前就說清楚。交給 yt-dlp 的話，它只會回報
    // 「Unknown postprocessor」或丟出 Python traceback，使用者看不出原因。
    if (needsEnsureH264(settings_) && pluginDirIfPresent().isEmpty()) {
        emit log(QStringLiteral("找不到 yt-dlp 外掛：%1\n"
                                "「相容優先」與輸出 mov 需要它。請把建置輸出的 yt-dlp-plugins 資料夾"
                                "放在 AnyVideoDownload.exe 旁邊（重新執行 build.bat 會自動複製），"
                                "或改用「畫質優先」並選 mov 以外的格式。")
                     .arg(pluginDir()),
                 "error");
        emit done(0, urls_.size(), false);
        return;
    }
    runNext();
}

void DownloadJob::cancel()
{
    cancelled_ = true;
    if (proc_) killTree(*proc_);
}

void DownloadJob::runNext()
{
    if (cancelled_ || index_ >= urls_.size()) {
        emit done(ok_, fail_, cancelled_);
        return;
    }
    const QString url = urls_[index_++];
    emit item(index_, urls_.size(), url);
    tried_.clear();
    tweaks_ = {};
    startUrl(url);
}

// 改寫成 yt-dlp 認得的網址（抖音的 modal_id、小紅書的用戶頁網址…），短連結先展開，再開始下載
void DownloadJob::startUrl(const QString& url)
{
    if (!resolver_) resolver_ = new LinkResolver(this);
    resolver_->resolve(url, [this](const QString& finalUrl, const QString& note) {
        if (cancelled_) { emit done(ok_, fail_, true); return; }
        if (!note.isEmpty()) emit log(note, "info");
        startAttempt(finalUrl);
    });
}

void DownloadJob::startAttempt(const QString& url)
{
    url_ = url;
    errors_.clear();
    buffer_.clear();
    lastPost_.invalidate();
    proc_ = new QProcess(this);
    configureProcess(*proc_);
    proc_->setProcessChannelMode(QProcess::MergedChannels);
    connect(proc_, &QProcess::readyReadStandardOutput, this, &DownloadJob::onOutput);
    connect(proc_, &QProcess::finished, this, &DownloadJob::onFinished);
    connect(proc_, &QProcess::errorOccurred, this, [this, url](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;  // 其他錯誤之後還會收到 finished
        emit log(QStringLiteral("無法執行 yt-dlp：%1").arg(proc_->errorString()), "error");
        emit log(QStringLiteral("失敗：%1").arg(url), "error");
        ++fail_;
        proc_->deleteLater();
        proc_ = nullptr;
        QTimer::singleShot(0, this, &DownloadJob::runNext);
    });
    proc_->setProperty("url", url);
    proc_->start(engine_.program,
                 engine_.args(buildDownloadArgs(settings_, ffmpegDir_, pluginDirIfPresent(), url, tweaks_)));
}

void DownloadJob::onOutput()
{
    buffer_ += proc_->readAllStandardOutput();
    for (const QString& line : takeLines(buffer_)) handleLine(line);
}

void DownloadJob::onFinished(int exitCode, QProcess::ExitStatus st)
{
    onOutput();
    if (!buffer_.isEmpty()) {
        handleLine(QString::fromUtf8(buffer_));
        buffer_.clear();
    }
    const QString url = proc_->property("url").toString();
    proc_->deleteLater();
    proc_ = nullptr;

    if (cancelled_) {
        emit done(ok_, fail_, true);
        return;
    }
    if (st == QProcess::NormalExit && exitCode == 0) {
        ++ok_;
        emit log(QStringLiteral("完成：%1").arg(url), "ok");
        runNext();
        return;
    }
    // 失敗：原因 yt-dlp 已經印過了。依原因換個方式再試，沒有辦法就給建議
    const QString kind = classifyError(errors_.join('\n'));
    QString message;
    if (planRetry(kind, tried_, tweaks_, message)) {
        emit log(message, "warn");
        QTimer::singleShot(kind == "network" ? 3000 : 0, this, [this, url] {
            if (cancelled_) emit done(ok_, fail_, true);
            else startAttempt(url);
        });
        return;
    }
    const Platform platform = platformOf(QUrl(url));
    const bool cookies = settings_.browser != QStringLiteral("不使用") && !settings_.browser.isEmpty();
    QString hint = platformHint(platform, kind, errors_.join('\n'), cookies);     // 這個平台特有的說明優先
    if (hint.isEmpty()) hint = errorHint(kind, settings_);
    if (!hint.isEmpty()) emit log(QStringLiteral("建議：") + hint, "warn");
    if (captureIsGoodFallback(platform) && !captureUrls_.contains(url)) captureUrls_ << url;
    ++fail_;
    emit log(QStringLiteral("失敗：%1").arg(url), "error");
    runNext();
}

void DownloadJob::handleLine(const QString& line)
{
    if (line.isEmpty()) return;
    if (line.startsWith("AVD-DL|")) {
        handleDownloadProgress(line.split('|'));
    } else if (line.startsWith("AVD-PP|")) {
        const QStringList f = line.split('|');
        if (f.value(1) == "started") emit status(ppLabel(f.value(2)) + QStringLiteral("…"));
    } else if (cancelled_) {
        // 程序被結束時的殘餘訊息沒有意義
    } else if (line.startsWith("[debug] ") || (line.startsWith("[download]") && line.contains('%'))) {
        // 除錯訊息略過；下載百分比已經有進度條
    } else if (line.startsWith("ERROR:")) {
        errors_ << line;
        emit log(line, "error");
    } else if (line.startsWith("WARNING:")) {
        emit log(line, "warn");
    } else {
        emit log(line, "info");
    }
}

void DownloadJob::handleDownloadProgress(const QStringList& f)
{
    // AVD-DL|status|downloaded|total|estimate|speed|eta|frag_index|frag_count|vcodec|acodec|pl_index|n_entries
    const QString status = f.value(1);
    if (status == "finished") {
        emit progress(100.0, QStringLiteral("下載完成，後處理中…"));
        return;
    }
    if (status != "downloading") return;
    if (lastPost_.isValid() && lastPost_.elapsed() < 150) return;  // 節流，避免洗版
    lastPost_.start();

    const double done = std::max(0.0, num(f.value(2)));
    double total = num(f.value(3));
    if (total <= 0) total = num(f.value(4));
    double pct = total > 0 ? done * 100 / total : -1;
    const double fragIndex = num(f.value(7)), fragCount = num(f.value(8));
    if (pct < 0 && fragCount > 0) pct = std::max(0.0, fragIndex) * 100 / fragCount;

    const QString part = f.value(9) == "none" ? QStringLiteral("音訊 ")
                       : f.value(10) == "none" ? QStringLiteral("影像 ") : QString();
    QString text = QStringLiteral("%1%2 / %3   %4/s   剩餘 %5")
                       .arg(part, fmtBytes(done), fmtBytes(total), fmtBytes(num(f.value(5))), fmtEta(num(f.value(6))));
    const double plIndex = num(f.value(11)), nEntries = num(f.value(12));
    if (plIndex > 0 && nEntries > 0)
        text = QStringLiteral("清單 %1/%2   ").arg(qint64(plIndex)).arg(qint64(nEntries)) + text;
    emit progress(pct, text);
}

// ─── 解析 ───────────────────────────────────────────────────────────────────

ProbeJob::ProbeJob(QString url, Settings s, Engine engine, QObject* parent)
    : QObject(parent), url_(std::move(url)), settings_(std::move(s)), engine_(std::move(engine))
{
}

void ProbeJob::start()
{
    auto* resolver = new LinkResolver(this);
    resolver->resolve(url_, [this, resolver](const QString& finalUrl, const QString& note) {
        resolver->deleteLater();
        if (!note.isEmpty()) emit log(note, "info");
        url_ = finalUrl;
        startProcess();
    });
}

void ProbeJob::startProcess()
{
    auto* p = new QProcess(this);
    configureProcess(*p);
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        emit done(false, QStringLiteral("解析失敗：無法執行 yt-dlp（%1）").arg(p->errorString()));
        p->deleteLater();
    });
    connect(p, &QProcess::finished, this, [this, p](int code, QProcess::ExitStatus st) {
        p->deleteLater();
        QString lastError;
        for (const QString& line : QString::fromUtf8(p->readAllStandardError()).split('\n')) {
            const QString l = line.trimmed();
            if (l.startsWith("WARNING:")) emit log(l, "warn");
            else if (l.startsWith("ERROR:")) lastError = l;
        }
        const QJsonObject info = QJsonDocument::fromJson(p->readAllStandardOutput()).object();
        if (st != QProcess::NormalExit || code != 0 || info.isEmpty()) {
            QString text = QStringLiteral("解析失敗：") +
                           (lastError.isEmpty() ? QStringLiteral("yt-dlp 結束代碼 %1").arg(code) : lastError);
            const QString hint = errorHint(classifyError(lastError), settings_);
            if (!hint.isEmpty()) text += QStringLiteral("\n建議：") + hint;
            emit done(false, text);
            return;
        }

        auto text = [&](const char* key) { return info.value(key).toString(); };
        QStringList lines;
        if (text("_type") == "playlist") {
            const auto n = info.value("entries").toArray().size();
            lines << QStringLiteral("播放清單：%1").arg(text("title").isEmpty() ? QStringLiteral("(無標題)") : text("title"));
            lines << QStringLiteral("共 %1 項").arg(n) +
                         (settings_.playlist ? QString() : QStringLiteral("　—　要全部下載請勾選「下載整個播放清單」"));
        } else {
            lines << QStringLiteral("標題：%1").arg(text("title").isEmpty() ? QStringLiteral("(無標題)") : text("title"));
            if (!text("uploader").isEmpty()) lines << QStringLiteral("上傳者：%1").arg(text("uploader"));
            if (info.value("duration").toDouble() > 0)
                lines << QStringLiteral("長度：%1").arg(fmtEta(info.value("duration").toDouble()));
            std::set<int, std::greater<int>> heights;
            for (const auto& fmt : info.value("formats").toArray()) {
                const int h = fmt.toObject().value("height").toInt();
                if (h > 0) heights.insert(h);
            }
            if (!heights.empty()) {
                QStringList hs;
                for (int h : heights) hs << QString::number(h) + "p";
                lines << QStringLiteral("可用畫質：") + hs.join(QStringLiteral("、"));
            }
            QString site = text("extractor_key");
            if (site.isEmpty()) site = text("extractor");
            lines << QStringLiteral("網站：%1").arg(site.isEmpty() ? "?" : site);
        }
        emit done(true, lines.join('\n'));
    });
    p->start(engine_.program, engine_.args(buildProbeArgs(settings_, url_)));
}

// ─── 更新下載核心 ───────────────────────────────────────────────────────────

UpdateJob::UpdateJob(Engine engine, QObject* parent) : QObject(parent), engine_(std::move(engine)) {}

void UpdateJob::start()
{
    QString program;
    QStringList args;
    if (engine_.kind == Engine::Kind::PyModule) {
        program = engine_.program;
        args = QStringList{"-3"} + kPipInstall;
    } else if (engine_.kind == Engine::Kind::Exe) {
        // pip 裝的 yt-dlp.exe 在 <Python>\Scripts 底下，要用 pip 更新；
        // 單一執行檔版（官方下載、winget）用自己的 -U 更新
        const QDir scripts = QFileInfo(engine_.program).dir();
        const QString python = QDir::cleanPath(scripts.filePath("../python.exe"));
        if (scripts.dirName().compare("Scripts", Qt::CaseInsensitive) == 0 && QFileInfo::exists(python)) {
            program = QDir::toNativeSeparators(python);
            args = kPipInstall;
        } else {
            program = engine_.program;
            args = {"-U"};
        }
    } else {
        const QString py = QStandardPaths::findExecutable("py");
        if (py.isEmpty()) {
            emit log(QStringLiteral("找不到 Python，無法自動安裝。請執行 winget install yt-dlp.yt-dlp，"
                                    "裝完重新開啟本程式。"), "error");
            emit done(-1);
            return;
        }
        program = py;
        args = QStringList{"-3"} + kPipInstall;
    }

    auto* p = new QProcess(this);
    configureProcess(*p);
    p->setProcessChannelMode(QProcess::MergedChannels);
    auto buffer = std::make_shared<QByteArray>();
    connect(p, &QProcess::readyReadStandardOutput, this, [this, p, buffer] {
        *buffer += p->readAllStandardOutput();
        for (const QString& line : takeLines(*buffer))
            if (!line.trimmed().isEmpty()) emit log(line.trimmed(), "info");
    });
    connect(p, &QProcess::errorOccurred, this, [this, p](QProcess::ProcessError e) {
        if (e != QProcess::FailedToStart) return;
        emit log(QStringLiteral("無法執行更新程式：%1").arg(p->errorString()), "error");
        p->deleteLater();
        emit done(-1);
    });
    connect(p, &QProcess::finished, this, [this, p, buffer](int code, QProcess::ExitStatus) {
        *buffer += p->readAllStandardOutput();
        buffer->append('\n');
        for (const QString& line : takeLines(*buffer))
            if (!line.trimmed().isEmpty()) emit log(line.trimmed(), "info");
        p->deleteLater();
        emit done(code);
    });
    emit log(QStringLiteral("> %1 %2").arg(program, args.join(' ')), "info");
    p->start(program, args);
}

}  // namespace avd
