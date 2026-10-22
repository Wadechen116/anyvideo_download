#include "guard.h"

#include <QDir>
#include <QStorageInfo>
#include <QTemporaryFile>

namespace avd {

namespace {

QString normPath(const QString& raw)
{
    QString out = raw.toLower();
    out.replace('\\', '/');
    while (out.size() > 1 && out.endsWith('/')) out.chop(1);
    // \\?\C:\... 與 \\.\C:\... 是同一個磁碟路徑的另一種寫法
    if (out.size() >= 4 && (out.startsWith(QLatin1String("//?/")) || out.startsWith(QLatin1String("//./")))) out.remove(0, 4);
    // WSL 的 /mnt/c/windows 就是 c:/windows
    if (out.size() >= 6 && out.startsWith(QLatin1String("/mnt/")) && out[5] >= 'a' && out[5] <= 'z' && (out.size() == 6 || out[6] == '/'))
        out = QString(out[5]) + ':' + out.mid(6);
    return out;
}

bool startsWithFolder(const QString& path, const QString& folder)
{
    return path == folder || (path.size() > folder.size() && path.startsWith(folder) && path[folder.size()] == '/');
}

}  // namespace

bool isAdminOnlyPath(const QString& raw)
{
    const QString p = normPath(raw.trimmed());
    if (p.isEmpty()) return true;                                   // 沒有路徑不是可以寫的地方
    if (p.size() >= 2 && p[1] == ':' && p[0] >= 'a' && p[0] <= 'z') {
        const QString rest = p.size() > 3 ? p.mid(3) : QString();
        if (rest.isEmpty()) return true;                            // 磁碟根目錄本身
        // 直接放在根目錄的檔案；根目錄底下自己建的資料夾（D:/影片）另外判斷
        if (!rest.contains('/') && rest.contains('.')) return true;
        for (const char* folder : {"windows", "program files", "program files (x86)", "programdata", "$recycle.bin", "system volume information", "recovery"})
            if (startsWithFolder(rest, QLatin1String(folder))) return true;
        return false;
    }
    for (const char* folder : {"/", "/etc", "/usr", "/bin", "/sbin", "/lib", "/lib32", "/lib64", "/opt", "/var", "/boot", "/sys", "/proc", "/dev", "/root", "/srv", "/snap"}) {
        const QString f = QLatin1String(folder);
        if (p == f || (f != QLatin1String("/") && startsWithFolder(p, f))) return true;
    }
    return false;
}

DirProbe probeOutDir(const QString& outDir)
{
    DirProbe probe;
    QTemporaryFile f(QDir(outDir).filePath(QStringLiteral(".avd_write_test_XXXXXX")));
    probe.writable = f.open();
    const QStorageInfo info(outDir);
    if (info.isValid() && info.isReady()) probe.free = info.bytesAvailable();
    return probe;
}

QList<GuardIssue> checkDownloadSettings(const Settings& s, const DirProbe& probe)
{
    QList<GuardIssue> issues;
    if (isAdminOnlyPath(s.outDir))
        issues.append({QStringLiteral("dir_admin_only"), s.outDir, true, {}});
    else if (!probe.writable)
        issues.append({QStringLiteral("dir_not_writable"), s.outDir, true, {}});
    else if (probe.free >= 0 && probe.free < kGuardMinFreeBytes)
        issues.append({QStringLiteral("disk_low"), QString::number(double(probe.free) / (1024.0 * 1024.0 * 1024.0), 'f', 1), false, {}});

    const bool video = s.mode == "video";
    const bool original = s.priority == "original";
    const QString& c = s.container;
    if (video && s.priority == "compat" && (c == "webm" || c == "avi"))
        issues.append({QStringLiteral("compat_not_kept"), c, false, {{"container", "mp4"}}});
    if (video && !original && s.embedSubs && !kSubsOk.contains(c))
        issues.append({QStringLiteral("subs_dropped"), c, false, {{"container", "mkv"}}});
    if (s.embedThumb) {
        if (video && original)
            issues.append({QStringLiteral("thumb_dropped"), QStringLiteral("original"), false, {{"embed_thumb", "false"}}});
        else if (video && !kThumbOk.contains(c))
            issues.append({QStringLiteral("thumb_dropped"), c, false, {{"container", "mkv"}}});
        else if (!video && !kThumbOk.contains(s.audioFormat))
            issues.append({QStringLiteral("thumb_dropped"), s.audioFormat, false,
                           s.audioFormat == "wav" ? QList<QPair<QString, QString>>{{"audio_format", "flac"}} : QList<QPair<QString, QString>>{{"embed_thumb", "false"}}});
    }
    return issues;
}

Settings applyGuardFixes(const Settings& s, const QList<GuardIssue>& issues)
{
    Settings out = s;
    for (const GuardIssue& i : issues)
        for (const auto& kv : i.fix) {
            if (kv.first == "container") out.container = kv.second;
            else if (kv.first == "audio_format") out.audioFormat = kv.second;
            else if (kv.first == "embed_thumb") out.embedThumb = kv.second == "true";
            else if (kv.first == "embed_subs") out.embedSubs = kv.second == "true";
        }
    return out;
}

QString describeGuardIssue(const GuardIssue& i)
{
    const QString& d = i.detail;
    if (i.id == "dir_admin_only")
        return QStringLiteral("儲存位置 %1 在需要管理員權限的位置（Windows、Program Files 等），程式不會寫入那裡。請選擇你自己的資料夾（例如「文件」或「影片」底下）。").arg(d);
    if (i.id == "dir_not_writable")
        return QStringLiteral("無法寫入儲存位置 %1：資料夾是唯讀、被 Windows「受控資料夾存取」（防勒索軟體）擋住，或權限不足。請換一個位置（例如桌面）。").arg(d);
    if (i.id == "disk_low")
        return QStringLiteral("儲存位置所在的磁碟只剩 %1 GB 可用空間。高畫質影片可能好幾 GB，空間不夠時下載到一半會失敗。").arg(d);
    if (i.id == "compat_not_kept")
        return QStringLiteral("「相容優先」承諾輸出 H.264 + AAC，但 %1 容器做不到（webm 裝不下 H.264，avi 一律轉成 MPEG-4 + MP3），結果不會是 H.264。要 H.264 請改用 mp4（畫質最高又能播）。").arg(d);
    if (i.id == "subs_dropped")
        return QStringLiteral("已勾選嵌入字幕，但 %1 容器不支援，字幕不會被嵌入。改用 mkv 可以嵌入（也不必重新編碼）。").arg(d);
    if (i.id == "thumb_dropped") {
        if (d == "original") return QStringLiteral("「原始品質（不轉檔）」要等網站給什麼才知道容器，所以不會嵌入封面；勾選的「嵌入封面」不會有作用。");
        if (d == "best") return QStringLiteral("音訊「原始（不轉檔）」不嵌入封面（格式要看網站給什麼才知道）；勾選的「嵌入封面」不會有作用。");
        if (d == "wav") return QStringLiteral("wav 不支援嵌入封面。改用 flac（同樣無損）可以嵌入。");
        return QStringLiteral("已勾選嵌入封面，但 %1 容器不支援，封面不會被嵌入。改用 mkv 可以嵌入。").arg(d);
    }
    return i.id;
}

}  // namespace avd
