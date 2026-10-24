// 下載前的設定防呆：按「開始下載」前先檢查哪些設定其實做不到、會悄悄被略過、或根本寫不進去。
//
// 每一項都是「確定有問題」才提醒（不是你刻意的選擇都要念一次），能修的附上修法，修法以「畫質最高又不損耗」為優先
// （例如改成 mkv：什麼都裝得下，只是重新封裝，不必重新編碼）。
//   致命（不能照原設定做）：dir_admin_only 儲存位置在需要管理員權限的地方、dir_not_writable 儲存位置寫不進去
//   提醒：disk_low 磁碟空間不到 2 GB、compat_not_kept 相容優先卻選了 webm／avi（做不到 H.264）、
//         cookies_missing 設了 cookies.txt 但檔案不存在（會被忽略；修法＝清除）、
//         subs_dropped／thumb_dropped 勾了嵌入字幕／封面但容器不支援（會悄悄不嵌）
// 規則與 py/anyvideo_download.py、csharp/Guard.cs 相同（tests/guard_flow.py 三個版本跑同一批案例）。
#pragma once

#include <QList>
#include <QPair>
#include <QString>

#include "settings.h"

namespace avd {

inline constexpr qint64 kGuardMinFreeBytes = 2LL * 1024 * 1024 * 1024;

struct GuardIssue {
    QString id;
    QString detail;
    bool fatal = false;
    QList<QPair<QString, QString>> fix;     // （設定名稱, 值）；空＝沒有自動修正
};

// 儲存位置的實際狀況：寫得進去嗎、還剩多少空間（-1＝不知道）。資料夾要已經存在。
struct DirProbe {
    bool writable = true;
    qint64 free = -1;
};
DirProbe probeOutDir(const QString& outDir);

// 需要管理員權限才能寫的地方（Windows 系統資料夾、Program Files、磁碟根目錄、Unix 的系統資料夾）。程式永遠不寫那裡。
bool isAdminOnlyPath(const QString& path);

QList<GuardIssue> checkDownloadSettings(const Settings& s, const DirProbe& probe);

// 把每個問題的修法套上去（依序，後面的蓋過前面的）。回傳新的設定，原本的不動。
Settings applyGuardFixes(const Settings& s, const QList<GuardIssue>& issues);

QString describeGuardIssue(const GuardIssue& issue);

}  // namespace avd
