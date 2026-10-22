#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>

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
    return s;
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
    };
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return;  // 存不了設定不該讓下載失敗
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.commit();
}

}  // namespace avd
