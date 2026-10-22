#include "platform.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QTimer>
#include <QUrlQuery>

namespace avd {

namespace {

bool hostIs(const QString& host, const QString& domain)
{
    return host == domain || host.endsWith(QLatin1Char('.') + domain);
}

QString lowerHost(const QUrl& u) { return u.host().toLower(); }

// 這些字元不會是網址的一部分，貼上的分享文字裡它們把網址和下一句話隔開
const QString kStopChars = QStringLiteral("\\s<>\"'`\u3000，。！？、；：（）【】「」『』《》〈〉…“”‘’");

}  // namespace

QString platformName(Platform p)
{
    switch (p) {
    case Platform::YouTube: return QStringLiteral("YouTube");
    case Platform::Douyin: return QStringLiteral("抖音");
    case Platform::TikTok: return QStringLiteral("TikTok");
    case Platform::XiaoHongShu: return QStringLiteral("小紅書");
    case Platform::Other: break;
    }
    return {};
}

Platform platformOf(const QUrl& url)
{
    const QString h = lowerHost(url);
    if (hostIs(h, "youtube.com") || hostIs(h, "youtu.be") || hostIs(h, "youtube-nocookie.com")) return Platform::YouTube;
    if (hostIs(h, "douyin.com") || hostIs(h, "iesdouyin.com") || hostIs(h, "douyinvod.com")) return Platform::Douyin;
    if (hostIs(h, "tiktok.com")) return Platform::TikTok;
    if (hostIs(h, "xiaohongshu.com") || hostIs(h, "xhslink.com") || hostIs(h, "xhscdn.com")) return Platform::XiaoHongShu;
    return Platform::Other;
}

QStringList extractUrls(const QString& text)
{
    static const QRegularExpression full(QStringLiteral("https?://[^%1]+").arg(kStopChars), QRegularExpression::CaseInsensitiveOption);
    // 分享文字常省略 https://：只認得這幾個已知的短連結網域，避免把一般文字當成網址
    static const QRegularExpression bare(QStringLiteral("(?<![/\\w.])((?:v\\.douyin\\.com|xhslink\\.com|vm\\.tiktok\\.com|vt\\.tiktok\\.com|www\\.xiaohongshu\\.com|www\\.douyin\\.com|www\\.tiktok\\.com|youtu\\.be)/[^%1]+)").arg(kStopChars),
                                         QRegularExpression::CaseInsensitiveOption);
    struct Hit { qsizetype pos; QString url; };
    QList<Hit> hits;
    for (auto it = full.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        hits << Hit{ m.capturedStart(), m.captured() };
    }
    for (auto it = bare.globalMatch(text); it.hasNext();) {
        const auto m = it.next();
        // 已經被上面（有 https:// 的）涵蓋就不重複算
        const bool covered = std::any_of(hits.begin(), hits.end(), [&](const Hit& h) { return m.capturedStart(1) >= h.pos && m.capturedStart(1) < h.pos + h.url.size(); });
        if (!covered) hits << Hit{ m.capturedStart(1), QStringLiteral("https://") + m.captured(1) };
    }
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.pos < b.pos; });
    QStringList out;
    for (Hit& h : hits) {
        QString u = h.url;
        while (!u.isEmpty() && QStringLiteral(".,;:!?)]}>*").contains(u.back())) u.chop(1);       // 句尾的標點不是網址
        if (QUrl(u).host().isEmpty() || out.contains(u)) continue;
        out << u;
    }
    return out;
}

QString douyinVideoId(const QUrl& url)
{
    static const QRegularExpression pathId(QStringLiteral("/(?:video|share/video|note|share/note)/(\\d+)"));
    const QString h = lowerHost(url);
    if (!hostIs(h, "douyin.com") && !hostIs(h, "iesdouyin.com")) return {};
    if (const auto m = pathId.match(url.path()); m.hasMatch()) return m.captured(1);
    const QString modal = QUrlQuery(url).queryItemValue(QStringLiteral("modal_id"));           // 精選、推薦、用戶頁裡點開的影片
    static const QRegularExpression digits(QStringLiteral("^\\d+$"));
    return digits.match(modal).hasMatch() ? modal : QString();
}

QString normalizeUrl(const QString& raw)
{
    const QUrl url(raw.trimmed());
    const QString host = lowerHost(url);
    const Platform p = platformOf(url);

    if (p == Platform::Douyin) {
        // note（圖文）yt-dlp 沒有解析器，不改寫；影片一律改成 douyin.com/video/<id>
        if (url.path().contains(QLatin1String("/note/"))) return raw.trimmed();
        const QString id = douyinVideoId(url);
        if (!id.isEmpty()) return QStringLiteral("https://www.douyin.com/video/") + id;
        return raw.trimmed();
    }
    if (p == Platform::XiaoHongShu && hostIs(host, "xiaohongshu.com")) {
        // /user/profile/<用戶>/<筆記>、/search_result/<筆記>、/explore/<筆記> → /explore/<筆記>；查詢參數（xsec_token）保留
        static const QRegularExpression note(QStringLiteral("/(?:user/profile/[0-9a-f]+|search_result|explore|discovery/item)/([0-9a-f]{16,})"));
        if (const auto m = note.match(url.path()); m.hasMatch()) {
            QUrl out;
            out.setScheme(QStringLiteral("https"));
            out.setHost(QStringLiteral("www.xiaohongshu.com"));
            out.setPath(QStringLiteral("/explore/") + m.captured(1));
            out.setQuery(QUrlQuery(url));
            return out.toString(QUrl::FullyEncoded);
        }
        return raw.trimmed();
    }
    return raw.trimmed();
}

bool isShortLink(const QUrl& url, const QSet<QString>& extraHosts)
{
    const QString h = lowerHost(url);
    if (extraHosts.contains(h)) return true;
    if (h == QLatin1String("v.douyin.com") || hostIs(h, "xhslink.com")) return true;
    if (h == QLatin1String("vm.tiktok.com") || h == QLatin1String("vt.tiktok.com")) return false;      // yt-dlp 自己會處理
    return false;
}

QString absoluteLocation(const QUrl& base, const QString& location)
{
    return base.resolved(QUrl(location.trimmed())).toString(QUrl::FullyEncoded);
}

bool captureIsGoodFallback(Platform p) { return p == Platform::Douyin || p == Platform::XiaoHongShu || p == Platform::TikTok; }

QString platformHint(Platform p, const QString& kind, const QString& errorText, bool usesBrowserCookies)
{
    if (p == Platform::Douyin) {
        if (errorText.contains(QLatin1String("Fresh cookies"), Qt::CaseInsensitive) || kind == QLatin1String("login"))
            return QStringLiteral("抖音要求「新的 cookies」（不一定要登入）：先用 Firefox 或 Chrome 開 douyin.com、隨便看一支影片讓網站發給瀏覽器 cookies，"
                                  "再到「登入狀態」選那個瀏覽器。") +
                   (usesBrowserCookies ? QStringLiteral("已經選了瀏覽器仍失敗：請先關閉瀏覽器再試（Chrome／Edge 開著時讀不到 cookies），或改用 Firefox。") : QString()) +
                   QStringLiteral("還是不行就用「從瀏覽器擷取」：瀏覽器能播，就能抓。");
        if (kind == QLatin1String("update") || errorText.contains(QLatin1String("Unsupported URL")))
            return QStringLiteral("這個抖音網址形式 yt-dlp 不支援（例如圖文 /note/）或網站改版了。可以改用「從瀏覽器擷取」。");
    }
    if (p == Platform::XiaoHongShu) {
        if (errorText.contains(QLatin1String("certificate"), Qt::CaseInsensitive) || errorText.contains(QLatin1String("SSL"))) 
            return QStringLiteral("連到小紅書時憑證驗證失敗：你目前的網路把 xiaohongshu.com 導向了別的伺服器（憑證不是小紅書的），"
                                  "所以不是影片或程式的問題。這是網路層的限制，程式沒辦法處理；在能正常連到小紅書的網路環境下就可以下載。");
        if (kind == QLatin1String("login") || kind == QLatin1String("update") || errorText.contains(QLatin1String("Unsupported URL")))
            return QStringLiteral("小紅書的網址要帶 xsec_token 參數才給內容：請貼「分享」複製出來的完整網址（含 ?xsec_token=…），不要自己刪掉後面的參數。"
                                  "短連結 xhslink.com 程式會自己展開。仍不行可以改用「從瀏覽器擷取」。");
    }
    if (p == Platform::TikTok && (kind == QLatin1String("impersonate") || kind == QLatin1String("update")))
        return QStringLiteral("TikTok 常擋非瀏覽器的連線：按「更新下載核心」安裝瀏覽器偽裝元件（curl_cffi）；仍不行可以改用「從瀏覽器擷取」。");
    if (p == Platform::YouTube && (kind == QLatin1String("bot") || kind == QLatin1String("login")))
        return QStringLiteral("YouTube 要求證明不是機器人：「登入狀態」選一個已登入 YouTube 的瀏覽器（Firefox 最可靠）。"
                              "另外請確認已安裝 deno（winget install DenoLand.Deno），YouTube 需要它才能取得完整格式。");
    return {};
}

LinkResolver::LinkResolver(QObject* parent) : QObject(parent), m_nam(new QNetworkAccessManager(this)) {}

void LinkResolver::resolve(const QString& url, std::function<void(const QString&, const QString&)> done)
{
    step(url, normalizeUrl(url), 0, std::move(done));
}

void LinkResolver::step(const QString& original, const QString& current, int hops, std::function<void(const QString&, const QString&)> done)
{
    const QUrl u(current);
    const Platform p = platformOf(u);
    auto finish = [&](const QString& finalUrl, const QString& note) {
        const QString n = !note.isEmpty() ? note
                        : (finalUrl != original && p != Platform::Other) ? QStringLiteral("辨識為%1：%2").arg(platformName(platformOf(QUrl(finalUrl))), finalUrl) : QString();
        done(finalUrl, n);
    };
    if (!isShortLink(u, m_extra)) { finish(current, {}); return; }
    if (hops >= 6) { finish(original, QStringLiteral("短連結轉址超過 6 次，放棄展開：%1").arg(original)); return; }

    QNetworkRequest req(u);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);          // 自己一步一步走，每一步都看看是不是已經是真正的網址
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("Mozilla/5.0 (iPhone; CPU iPhone OS 17_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.0 Mobile/15E148 Safari/604.1"));
    req.setTransferTimeout(15000);
    QNetworkReply* reply = m_nam->get(req);
    connect(reply, &QNetworkReply::finished, this, [this, reply, original, u, hops, done]() mutable {
        reply->deleteLater();
        const QString location = QString::fromUtf8(reply->rawHeader("Location"));
        if (location.isEmpty()) {
            const QString why = reply->error() != QNetworkReply::NoError ? reply->errorString() : QStringLiteral("沒有轉址");
            done(original, QStringLiteral("短連結展開失敗（%1）：%2").arg(why, original));
            return;
        }
        const QString next = normalizeUrl(absoluteLocation(u, location));
        step(original, next, hops + 1, std::move(done));
    });
}

}  // namespace avd
