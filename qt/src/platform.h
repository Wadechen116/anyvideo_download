// 影音平台的網址處理：從分享文字裡找出網址、辨認平台、改寫成 yt-dlp 認得的形式、解開短連結。
//
// 為什麼需要：使用者貼的常常不是乾淨的網址。抖音與小紅書的「分享」會複製一整段文字（標題、表情符號、
// 「復制打開抖音」之類的話，中間夾著 https://v.douyin.com/xxxx/ 這樣的短連結）；瀏覽器網址列的抖音網址常是
// douyin.com/jingxuan?modal_id=123，小紅書常是 xiaohongshu.com/user/profile/<用戶>/<筆記>。yt-dlp 的解析器只認
// douyin.com/video/<id> 與 xiaohongshu.com/explore/<id>，其他形式要先改寫，否則一律「Unsupported URL」。
#pragma once

#include <QNetworkAccessManager>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <functional>

namespace avd {

enum class Platform { Other, YouTube, Douyin, TikTok, XiaoHongShu };

QString platformName(Platform p);                 // "YouTube"、"抖音"、"TikTok"、"小紅書"，其他回傳空字串
Platform platformOf(const QUrl& url);

// 一段文字裡所有的網址（依出現順序、不重複）。去掉網址後面黏著的標點（含全形的 。，！？）、「」等。
// 沒有 http(s):// 的 v.douyin.com/xxx、xhslink.com/xxx、vm.tiktok.com/xxx 也算，會補上 https://。
QStringList extractUrls(const QString& text);

// 改寫成 yt-dlp 認得的形式；不需要改寫的原樣回傳。小紅書網址裡的 xsec_token 等參數必須保留（沒有它網站不給內容）。
QString normalizeUrl(const QString& url);

// 短連結：要先跟著轉址走，才知道真正的影片網址（v.douyin.com、xhslink.com）。
bool isShortLink(const QUrl& url, const QSet<QString>& extraHosts = {});

// 轉址走完之後的最終網址，或轉址的第一步 Location（相對網址以 base 補全）。
QString absoluteLocation(const QUrl& base, const QString& location);

// 抖音網址裡的影片 id（douyin.com/video/ID、?modal_id=ID、iesdouyin.com/share/video/ID）；沒有回傳空字串。
QString douyinVideoId(const QUrl& url);

// 這個平台下載失敗時，使用者該知道的事。kind 是錯誤分類（見 engine.h 的 classifyError）；沒有特別的話回傳空字串。
QString platformHint(Platform p, const QString& kind, const QString& errorText, bool usesBrowserCookies, bool usesCookiesFile = false);

// 這個平台的下載失敗後，改用「從瀏覽器擷取」常常有效（yt-dlp 的解析器常因網站改版而壞）。
bool captureIsGoodFallback(Platform p);

// 把貼上的網址變成可以交給 yt-dlp 的網址：先改寫（normalizeUrl），是短連結（v.douyin.com、xhslink.com）就跟著轉址走到
// 真正的網址再改寫一次。轉址用手機瀏覽器的身分（分享連結是給手機開的，電腦版身分常被導去登入頁）。
class LinkResolver : public QObject {
    Q_OBJECT
public:
    explicit LinkResolver(QObject* parent = nullptr);
    void setExtraShortHosts(const QSet<QString>& hosts) { m_extra = hosts; }     // 測試用：讓本機的伺服器也當短連結

    // done(最終網址, 說明)：說明是給紀錄區看的一行（沒有話說時空白）。展開失敗時最終網址是原來的，說明講原因。
    void resolve(const QString& url, std::function<void(const QString& finalUrl, const QString& note)> done);

private:
    void step(const QString& original, const QString& current, int hops, std::function<void(const QString&, const QString&)> done);
    QNetworkAccessManager* m_nam;
    QSet<QString> m_extra;
};

} // namespace avd
