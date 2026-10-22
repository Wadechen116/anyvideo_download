// AnyVideo Download（Qt 版）進入點。
//
// 一般直接開啟視窗。另有給自動測試用的無介面模式，結果印到標準輸出（UTF-8、Tab 分隔）：
//
//   AnyVideoDownload.exe --headless args     <settings.json> <url>     印出 yt-dlp 參數，一行一個
//   AnyVideoDownload.exe --headless download <settings.json> <url>... 走和視窗相同的下載流程
//   AnyVideoDownload.exe --headless probe    <settings.json> <url>     解析網址
#include <QApplication>
#include <QCoreApplication>
#include <QTimer>
#include <QFile>
#include <QDir>
#include <QFont>

#include <cstdio>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

#include "capture.h"
#include "platform.h"
#include "engine.h"
#include "mainwindow.h"

using namespace avd;

namespace {

void out(const QString& line)
{
    const QByteArray b = (line + "\n").toUtf8();
    fwrite(b.constData(), 1, static_cast<size_t>(b.size()), stdout);
    fflush(stdout);
}

int headless(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
#ifdef Q_OS_WIN
    _setmode(_fileno(stdout), _O_BINARY);  // 不要把 \n 轉成 \r\n
#endif
    const QStringList a = app.arguments();
    // 測試用：不需要設定檔的兩個指令
    //   urls <文字檔>            印出從文字裡找到的每個網址：「平台<Tab>改寫後的網址」
    //   resolve <網址> [主機...]  展開短連結（額外列出的主機也當短連結，讓測試能用本機伺服器）
    if (a.size() >= 4 && a[2] == "urls") {
        QFile f(a[3]);
        if (!f.open(QIODevice::ReadOnly)) { out("error\tcannot open"); return 2; }
        for (const QString& u : extractUrls(QString::fromUtf8(f.readAll())))
            out(platformName(platformOf(QUrl(normalizeUrl(u)))) + "\t" + normalizeUrl(u));
        return 0;
    }
    if (a.size() >= 4 && a[2] == "resolve") {
        LinkResolver resolver;
        QSet<QString> hosts;
        for (int i = 4; i < a.size(); ++i) hosts.insert(a[i]);
        resolver.setExtraShortHosts(hosts);
        QTimer::singleShot(30000, &app, &QCoreApplication::quit);
        resolver.resolve(a[3], [&](const QString& finalUrl, const QString& note) {
            out("final\t" + finalUrl);
            out("note\t" + note);
            app.quit();
        });
        app.exec();
        return 0;
    }
    if (a.size() < 5) {
        out("usage: --headless args|download|probe <settings.json> <url>...");
        return 2;
    }
    const QString cmd = a[2];
    const Settings s = Settings::loadFrom(a[3]);
    const QStringList urls = a.mid(4);

    if (cmd == "args") {
        for (const QString& arg : buildDownloadArgs(s, findFfmpegDir(), pluginDir(), urls[0])) out(arg);
        return 0;
    }

    if (cmd == "capture") {  // 測試用：開瀏覽器 25 秒，印出抓到的影音網址
        CaptureSession session;
        QObject::connect(&session, &CaptureSession::status, [](const QString& t) { out("status\t" + t); });
        QObject::connect(&session, &CaptureSession::found, [](const MediaCandidate& c) {
            out(QString("found\t%1\t%2\t%3\t%4").arg(c.kind, c.mime).arg(c.size).arg(c.url.left(140)));
        });
        QString err;
        if (!session.start(urls[0], &err)) {
            out("error\t" + err);
            return 1;
        }
        QTimer::singleShot(25000, &app, &QCoreApplication::quit);
        return app.exec();
    }

    const Engine engine = detectEngine();
    if (!engine.ok()) {
        out("log\terror\t找不到下載核心 yt-dlp");
        return 3;
    }

    if (cmd == "probe") {
        ProbeJob job(urls[0], s, engine);
        int rc = 1;
        QObject::connect(&job, &ProbeJob::log, [](const QString& t, const QString& tag) { out("log\t" + tag + "\t" + t); });
        QObject::connect(&job, &ProbeJob::done, [&](bool ok, const QString& text) {
            out(QString("probe\t%1\t").arg(ok ? "ok" : "fail") + QString(text).replace('\n', " | "));
            rc = ok ? 0 : 1;
            app.quit();
        });
        job.start();
        app.exec();
        return rc;
    }

    if (cmd == "download") {
        DownloadJob job(urls, s, engine, findFfmpegDir());
        int rc = 1;
        QObject::connect(&job, &DownloadJob::log, [](const QString& t, const QString& tag) { out("log\t" + tag + "\t" + t); });
        QObject::connect(&job, &DownloadJob::item, [](int i, int n, const QString& url) {
            out(QString("item\t%1\t%2\t%3").arg(i).arg(n).arg(url));
        });
        QObject::connect(&job, &DownloadJob::status, [](const QString& t) { out("status\t" + t); });
        QObject::connect(&job, &DownloadJob::done, [&](int ok, int fail, bool cancelled) {
            out(QString("done\t%1\t%2\t%3").arg(ok).arg(fail).arg(cancelled ? 1 : 0));
            rc = fail == 0 && !cancelled ? 0 : 1;
            app.quit();
        });
        job.start();
        app.exec();
        return rc;
    }

    out("unknown command: " + cmd);
    return 2;
}

}  // namespace

int main(int argc, char* argv[])
{
    if (argc > 1 && QString::fromLocal8Bit(argv[1]) == "--headless") return headless(argc, argv);

    QApplication app(argc, argv);
    if (argc > 3 && QString::fromLocal8Bit(argv[1]) == "--selftest-live") {
        // --selftest-live <網址> <png> [輸出資料夾]
        avd::CaptureDialog dlg(avd::detectEngine(), avd::findFfmpegDir(), argc > 4 ? QString::fromLocal8Bit(argv[4]) : QDir::tempPath(), QString());
        return dlg.selfTestLive(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3])) ? 0 : 1;
    }
    app.setApplicationName(kAppName);
    app.setApplicationVersion(kAppVersion);
    // 不指定 fusion：Qt 6.5 的 fusion 在 Windows 深色模式下，未勾選的核取方塊會畫成實心藍色，
    // 看起來像已勾選。用預設的 Windows 原生樣式。
    QFont font("Microsoft JhengHei UI", 10);
    app.setFont(font);

    MainWindow w;
    w.show();
    return app.exec();
}
