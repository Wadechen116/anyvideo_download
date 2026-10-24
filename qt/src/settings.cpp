#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QCoreApplication>
#include <QDateTime>
#include <algorithm>

namespace avd {

QString ppLabel(const QString& name)
{
    static const QHash<QString, QString> labels = {
        {"Merger", QStringLiteral("合併影像與聲音")},
        {"VideoConvertor", QStringLiteral("轉換影片格式")},
        {"VideoRemuxer", QStringLiteral("重新封裝")},
        {"ExtractAudio", QStringLiteral("轉出音訊")},
        {"Metadata", QStringLiteral("寫入標題等資訊")},
        {"EmbedThumbnail", QStringLiteral("嵌入封面")},
        {"EmbedSubtitle", QStringLiteral("嵌入字幕")},
        {"MoveFiles", QStringLiteral("整理檔案")},
        {"EnsureH264", QStringLiteral("轉成相容編碼 H.264（比較慢）")},
    };
    return labels.value(name, name);
}

QString valueOf(const QList<Choice>& choices, const QString& label, const QString& fallback)
{
    for (const auto& c : choices)
        if (c.label == label) return c.value;
    return fallback;
}

QString labelOf(const QList<Choice>& choices, const QString& value, const QString& fallback)
{
    for (const auto& c : choices)
        if (c.value == value) return c.label;
    return fallback;
}

bool hasLabel(const QList<Choice>& choices, const QString& label)
{
    for (const auto& c : choices)
        if (c.label == label) return true;
    return false;
}

QString settingsPath()
{
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty()) base = QDir::homePath();
    return QDir(base).filePath("anyvideo_download/settings.json");
}

namespace {

constexpr qint64 kCookieMaxBytes = 5 * 1024 * 1024;

bool looksLikeNetscape(const QString& head)
{
    return head.contains(QLatin1String("Netscape HTTP Cookie File")) || head.contains(QLatin1String("HTTP Cookie File"));
}

bool hasCookieLine(const QString& text)
{
    const QStringList lines = text.split('\n');
    for (QString line : lines) {
        if (line.endsWith('\r')) line.chop(1);
        if (line.trimmed().isEmpty()) continue;
        if (line.startsWith(QLatin1String("#HttpOnly_"))) line = line.mid(10);
        if (line.startsWith('#')) continue;
        if (line.count('\t') == 6) return true;
    }
    return false;
}

QString decodeUtf8(const QByteArray& b)
{
    QString t = QString::fromUtf8(b);   // 壞掉的位元組換成 U+FFFD，不會失敗
    if (t.startsWith(QChar(0xFEFF))) t.remove(0, 1);
    return t;
}

struct CookieCand {
    QString path;
    bool douyin;
    bool named;
    qint64 mtime;
};

}  // namespace

bool cookieFileHasDouyin(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray b = f.read(kCookieMaxBytes);
    return b.contains("douyin.com") || b.contains("tiktok.com");
}

QStringList detectCookieFiles(const QStringList& dirs)
{
    QList<CookieCand> found;
    QSet<QString> seen;
    for (const QString& d : dirs) {
        if (d.trimmed().isEmpty()) continue;
        const QDir dir(d);
        if (!dir.exists()) continue;
        const QFileInfoList files = dir.entryInfoList(QStringList() << "*.txt", QDir::Files | QDir::NoDotAndDotDot);
        for (const QFileInfo& fi : files) {
            if (fi.size() > kCookieMaxBytes) continue;
            const QString abs = QDir::cleanPath(fi.absoluteFilePath());
#ifdef Q_OS_WIN
            const QString key = abs.toLower();
#else
            const QString key = abs;
#endif
            if (seen.contains(key)) continue;
            QFile f(abs);
            if (!f.open(QIODevice::ReadOnly)) continue;
            const QByteArray all = f.read(kCookieMaxBytes);
            f.close();
            const QString head = decodeUtf8(all.left(4096));
            if (!looksLikeNetscape(head) && !hasCookieLine(decodeUtf8(all))) continue;
            seen.insert(key);
            found.append({QDir::toNativeSeparators(abs), all.contains("douyin.com") || all.contains("tiktok.com"),
                          fi.fileName().contains(QLatin1String("cookie"), Qt::CaseInsensitive),
                          fi.lastModified().toMSecsSinceEpoch()});
        }
    }
    std::sort(found.begin(), found.end(), [](const CookieCand& a, const CookieCand& b) {
        if (a.douyin != b.douyin) return a.douyin;
        if (a.named != b.named) return a.named;
        if (a.mtime != b.mtime) return a.mtime > b.mtime;
        return a.path < b.path;
    });
    QStringList out;
    for (const CookieCand& c : found) out << c.path;
    return out;
}

QStringList defaultCookieDirs(const QString& outDir)
{
    QStringList dirs;
    const QString profile = qEnvironmentVariable("USERPROFILE").isEmpty() ? QDir::homePath() : qEnvironmentVariable("USERPROFILE");
    for (const char* sub : {"Downloads", "Desktop", "Documents"}) dirs << QDir(profile).filePath(sub);
    const QString appdata = qEnvironmentVariable("APPDATA");
    if (!appdata.isEmpty()) dirs << QDir(appdata).filePath("anyvideo_download");
    if (!outDir.trimmed().isEmpty()) dirs << outDir.trimmed();
    dirs << QCoreApplication::applicationDirPath();
    return dirs;
}

Settings::Settings() : outDir(QDir::toNativeSeparators(QDir::homePath() + "/Downloads")) {}

Settings Settings::load() { return loadFrom(settingsPath()); }

Settings Settings::loadFrom(const QString& path)
{
    Settings s;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return s;
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();

    auto str = [&](const char* key, QString& field) {
        if (o.value(key).isString()) field = o.value(key).toString();
    };
    auto flag = [&](const char* key, bool& field) {
        if (o.value(key).isBool()) field = o.value(key).toBool();
    };
    str("out_dir", s.outDir);
    str("mode", s.mode);
    str("container", s.container);
    str("quality", s.quality);
    str("priority", s.priority);
    str("audio_format", s.audioFormat);
    str("audio_bitrate", s.audioBitrate);
    flag("playlist", s.playlist);
    flag("embed_meta", s.embedMeta);
    flag("embed_thumb", s.embedThumb);
    flag("embed_subs", s.embedSubs);
    str("browser", s.browser);
    str("cookies_file", s.cookiesFile);
    return s;
}

QString Settings::usableCookiesFile() const
{
    const QString p = cookiesFile.trimmed();
    return (!p.isEmpty() && QFileInfo(p).isFile()) ? p : QString();
}

void Settings::save() const
{
    const QString path = settingsPath();
    QDir().mkpath(QFileInfo(path).absolutePath());
    // 鍵名與 Python 版相同（snake_case），三個版本共用這個檔
    QJsonObject o{
        {"out_dir", outDir},         {"mode", mode},
        {"container", container},    {"quality", quality},
        {"priority", priority},      {"audio_format", audioFormat},
        {"audio_bitrate", audioBitrate}, {"playlist", playlist},
        {"embed_meta", embedMeta},   {"embed_thumb", embedThumb},
        {"embed_subs", embedSubs},   {"browser", browser},
        {"cookies_file", cookiesFile},
    };
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;  // 存不了設定不該讓下載失敗
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.commit();
}

}  // namespace avd
