/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "readermode.h"

#include "engineinterface.h"
#include "webview.h"

#include <qapplication.h>
#include <qfile.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qpalette.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qvariant.h>
#include <qwebchannel.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>

#ifdef ARORA_RUSTCORE
#include "rustcore.h"

#include <qboxlayout.h>
#include <qevent.h>
#include <qlabel.h>
#include <qtoolbutton.h>
#include <qwebengineview.h>
#include <qwidget.h>

namespace {

// The reader surface's page: it only ever displays the sanitized
// article document the core produced.  Link clicks leave the reader
// and navigate the real page — the load drops the overlay the same
// way any navigation does; in-document anchors stay inside.
class ReaderOverlayPage : public QWebEnginePage
{
public:
    ReaderOverlayPage(QWebEngineProfile *profile, const QUrl &baseUrl,
                      WebView *view, QObject *parent)
        : QWebEnginePage(profile, parent)
        , m_baseUrl(baseUrl)
        , m_view(view)
    {
    }

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type,
                                 bool isMainFrame) override
    {
        if (!isMainFrame)
            return false;
        if (type == NavigationTypeLinkClicked) {
            if (url.hasFragment()
                && url.adjusted(QUrl::RemoveFragment)
                    == m_baseUrl.adjusted(QUrl::RemoveFragment)) {
                return true;
            }
            if (m_view
                && (url.scheme() == QLatin1String("http")
                    || url.scheme() == QLatin1String("https")
                    || url.scheme() == QLatin1String("file"))) {
                m_view->loadUrl(url);
            }
            return false;
        }
        return true;
    }

private:
    QUrl m_baseUrl;
    QPointer<WebView> m_view;
};

// The document the overlay renders — the same structure and styling
// the JS overlay's shadow root carried, minus the in-chrome bar (the
// Rust path's bar is real Qt widgets).  Content arrives sanitized
// from the core; title/byline are escaped text.
QString readerDocument(const QVariantMap &article, const QString &theme)
{
    QStringList bylineParts;
    const QString byline = article.value(QLatin1String("byline")).toString();
    const QString siteName =
        article.value(QLatin1String("siteName")).toString();
    if (!byline.isEmpty())
        bylineParts << byline.toHtmlEscaped();
    if (!siteName.isEmpty())
        bylineParts << siteName.toHtmlEscaped();

    QString doc = QStringLiteral(
        "<!doctype html><html><head><meta charset=\"utf-8\"><style>"
        ":root{--fs:19px}"
        "body{margin:0;background:var(--bg);color:var(--fg);"
        "font-family:Georgia,'Times New Roman',serif;"
        "font-size:var(--fs);line-height:1.65}"
        "body.theme-light{--bg:#f8f6f1;--fg:#23231f;--link:#0b57a4;"
        "--bar-fg:#4a463d;--byline:#6d675c}"
        "body.theme-dark{--bg:#1c1c1e;--fg:#e6e2da;--link:#8ab4f8;"
        "--bar-fg:#b9b4aa;--byline:#9a958b}"
        "article{display:block;max-width:42em;margin:0 auto;"
        "padding:1.5em 1em 4em}"
        "h1.title{font-size:1.7em;line-height:1.25;margin:0 0 .4em}"
        ".byline{color:var(--byline);font-family:system-ui,sans-serif;"
        "font-size:.78em;margin-bottom:1.6em}"
        ".content a{color:var(--link)}"
        ".content img,.content video,.content svg{max-width:100%;"
        "height:auto}"
        ".content pre,.content code{font-family:monospace;"
        "font-size:.85em;white-space:pre-wrap}"
        ".content blockquote{margin:1em 0;padding-left:1em;"
        "border-left:3px solid var(--bar-fg);opacity:.85}"
        ".content table{border-collapse:collapse;max-width:100%;"
        "display:block;overflow-x:auto}"
        ".content td,.content th{border:1px solid var(--bar-fg);"
        "padding:4px 8px}"
        ".content figcaption,.content caption{font-size:.8em;"
        "color:var(--byline)}"
        ".content hr{border:0;border-top:1px solid var(--bar-fg)}"
        "</style></head><body class=\"theme-")
        + theme + QStringLiteral("\">")
        + QStringLiteral("<article><h1 class=\"title\">")
        + article.value(QLatin1String("title")).toString().toHtmlEscaped()
        + QStringLiteral("</h1>");
    if (!bylineParts.isEmpty()) {
        doc += QStringLiteral("<div class=\"byline\">")
            + bylineParts.join(QStringLiteral(" \u2014 "))
            + QStringLiteral("</div>");
    }
    doc += QStringLiteral("<div class=\"content\">")
        + article.value(QLatin1String("content")).toString()
        + QStringLiteral("</div></article></body></html>");
    return doc;
}

} // namespace
#endif // ARORA_RUSTCORE

void ReaderBridge::notifyExited()
{
    if (m_readerMode)
        m_readerMode->scriptExited();
}

void ReaderBridge::setPreference(const QString &key, const QVariant &value)
{
    if (m_readerMode)
        m_readerMode->scriptPreference(key, value);
}

ReaderMode::ReaderMode(WebView *view)
    : QObject(view)
    , m_view(view)
    , m_bridge(new ReaderBridge(this))
    , m_active(false)
    , m_available(false)
    , m_busy(false)
{
    // The bridge rides the page's existing channel (same registration
    // point as "external"/"aroraAutofill").  Tor pages carry no channel
    // at all — the overlay still works there, minus persistence and
    // JS-side exit notification.
    if (QWebChannel *channel = m_view->page()->webChannel())
        channel->registerObject(QLatin1String("aroraReader"), m_bridge);

    connect(m_view->page(), &QWebEnginePage::loadStarted,
            this, &ReaderMode::onLoadStarted);
    connect(m_view, &QWebEngineView::loadFinished,
            this, &ReaderMode::onLoadFinished);
#ifdef ARORA_RUSTCORE
    // The Rust overlay tracks the view's geometry.
    m_view->installEventFilter(this);
#endif
}

int ReaderMode::fontSize()
{
    QSettings settings;
    return qBound(12, settings.value(QLatin1String("reader/fontSize"), 19)
                      .toInt(), 30);
}

QString ReaderMode::theme()
{
    QSettings settings;
    const QString stored =
        settings.value(QLatin1String("reader/theme")).toString();
    if (stored == QLatin1String("light") || stored == QLatin1String("dark"))
        return stored;
    // Unset = follow the application theme.
    return QApplication::palette().color(QPalette::Window).value() < 128
        ? QStringLiteral("dark") : QStringLiteral("light");
}

QString ReaderMode::scriptBundle() const
{
    static const QString bundle = [] {
        QString source;
        const char *files[] = {
            ":/Readability-readerable.js",
            ":/Readability.js",
            ":/reader.js",
        };
        for (const char *path : files) {
            QFile file(QString::fromLatin1(path));
            if (!file.open(QIODevice::ReadOnly)) {
                qWarning() << "ReaderMode:" << "Unable to open" << path;
                continue;
            }
            source += QString::fromUtf8(file.readAll());
            source += QLatin1Char('\n');
        }
        return source;
    }();
    return bundle;
}

void ReaderMode::setActive(bool active)
{
#ifdef ARORA_RUSTCORE
    if (!active)
        closeOverlay();
#endif
    if (m_active == active)
        return;
    m_active = active;
    emit activeChanged(active);
    emit availableChanged(isAvailable());
}

#ifdef ARORA_RUSTCORE
bool ReaderMode::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_view && m_overlay
        && event->type() == QEvent::Resize) {
        m_overlay->setGeometry(m_view->rect());
    }
    return QObject::eventFilter(watched, event);
}
#else
bool ReaderMode::eventFilter(QObject *watched, QEvent *event)
{
    return QObject::eventFilter(watched, event);
}
#endif

void ReaderMode::setAvailable(bool available)
{
    available = available || m_active;
    if (m_available == available)
        return;
    m_available = available;
    emit availableChanged(available);
}

void ReaderMode::onLoadStarted()
{
    // A navigation drops the overlay with the old document.
    m_busy = false;
    setActive(false);
    setAvailable(false);
}

void ReaderMode::onLoadFinished(bool ok)
{
    if (ok && !m_active)
        probe();
}

void ReaderMode::scriptExited()
{
    // The in-overlay "Exit reader view" button already removed the
    // host element — just adopt the state change.
    setActive(false);
}

void ReaderMode::scriptPreference(const QString &key, const QVariant &value)
{
    QSettings settings;
    if (key == QLatin1String("fontSize")) {
        settings.setValue(QLatin1String("reader/fontSize"),
                          qBound(12, value.toInt(), 30));
    } else if (key == QLatin1String("theme")) {
        const QString theme = value.toString();
        if (theme == QLatin1String("light") || theme == QLatin1String("dark"))
            settings.setValue(QLatin1String("reader/theme"), theme);
    }
}

void ReaderMode::runDriver(
        const QString &call,
        const std::function<void(const QVariant &)> &callback)
{
    Engine::Page *page = m_view ? m_view->enginePage() : nullptr;
    if (!page) {
        if (callback)
            callback(QVariant());
        return;
    }
    const QString program = scriptBundle() + call;
    // JSCTL/SECLVL: the lift keeps the driver reaching pages whose
    // scripts are blocked — Engine::Page::runJavaScriptLifted owns
    // the JavascriptEnabled gate juggling.
    page->runJavaScriptLifted(program, callback);
}

void ReaderMode::probe()
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page)
        return;
    const QString scheme = page->url().scheme();
    if (scheme != QLatin1String("http") && scheme != QLatin1String("https")
        && scheme != QLatin1String("file")) {
        setAvailable(false);
        return;
    }
    QPointer<ReaderMode> self(this);
#ifdef ARORA_RUSTCORE
    // The Rust probe runs isProbablyReaderable's equivalent over the
    // serialized DOM — no script eval in the page world.
    collectHtml([self](const QString &html) {
        if (!self)
            return;
        if (html.isEmpty()) {
            self->setAvailable(false);
            return;
        }
        const QByteArray utf8 = html.toUtf8();
        self->setAvailable(rc_readability_probe(
            reinterpret_cast<const uint8_t *>(utf8.constData()),
            static_cast<size_t>(utf8.size())) > 0);
    });
#else
    runDriver(QLatin1String("window.__aroraReader.probe();"),
            [self](const QVariant &result) {
        if (self)
            self->setAvailable(result.toBool());
    });
#endif
}

#ifdef ARORA_RUSTCORE
// The serialized-DOM grab rides the same JavaScript-injection channel
// runJavaScript uses, so it needs the same JavascriptEnabled lift as
// runDriver when page scripts are blocked (JSCTL/Safer tiers).
void ReaderMode::collectHtml(
        const std::function<void(const QString &)> &callback)
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page) {
        callback(QString());
        return;
    }
    QWebEngineSettings *settings = page->settings();
    if (settings->testAttribute(QWebEngineSettings::JavascriptEnabled)) {
        page->toHtml(callback);
        return;
    }
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    QPointer<QWebEnginePage> livePage(page);
    QTimer::singleShot(700, this,
            [livePage, callback]() {
        if (!livePage)
            return;
        livePage->toHtml(
                [callback, livePage](const QString &html) {
            callback(html);
            QTimer::singleShot(400, qApp, [livePage]() {
                if (livePage) {
                    livePage->settings()->setAttribute(
                        QWebEngineSettings::JavascriptEnabled, false);
                }
            });
        });
    });
    // Fallback restore if the toHtml callback never arrives.
    QTimer::singleShot(5000, this, [livePage]() {
        if (livePage)
            livePage->settings()->setAttribute(
                QWebEngineSettings::JavascriptEnabled, false);
    });
}
#endif

#ifdef ARORA_RUSTCORE
void ReaderMode::showOverlay(const QVariantMap &article)
{
    closeOverlay();
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page)
        return;

    const QUrl baseUrl = page->url();
    const QString startTheme = theme();

    QWidget *overlay = new QWidget(m_view);
    overlay->setObjectName(QLatin1String("aroraReaderOverlay"));
    QVBoxLayout *layout = new QVBoxLayout(overlay);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    // The chrome bar — real widgets, so it needs no channel and
    // persists preferences on tor pages too.
    QWidget *bar = new QWidget(overlay);
    bar->setObjectName(QLatin1String("aroraReaderBar"));
    const bool dark = startTheme == QLatin1String("dark");
    bar->setStyleSheet(QStringLiteral(
        "#aroraReaderBar{background:%1;color:%2}"
        "#aroraReaderBar QLabel{font:13px system-ui;opacity:.7}"
        "#aroraReaderBar QToolButton{font:13px system-ui;color:%2;"
        "background:transparent;border:1px solid transparent;"
        "border-radius:4px;padding:3px 8px}"
        "#aroraReaderBar QToolButton:hover{"
        "background:rgba(127,127,127,.18);"
        "border-color:rgba(127,127,127,.35)}")
        .arg(dark ? QLatin1String("#262628") : QLatin1String("#efece4"),
             dark ? QLatin1String("#b9b4aa") : QLatin1String("#4a463d")));
    QHBoxLayout *barLayout = new QHBoxLayout(bar);
    barLayout->setContentsMargins(16, 6, 16, 6);
    QLabel *tag = new QLabel(tr("Reader"), bar);
    QToolButton *fontDec = new QToolButton(bar);
    fontDec->setObjectName(QLatin1String("aroraReaderFontDec"));
    fontDec->setText(QStringLiteral("A\u2212"));
    fontDec->setToolTip(tr("Smaller text"));
    QToolButton *fontInc = new QToolButton(bar);
    fontInc->setObjectName(QLatin1String("aroraReaderFontInc"));
    fontInc->setText(QStringLiteral("A+"));
    fontInc->setToolTip(tr("Larger text"));
    QToolButton *themeBtn = new QToolButton(bar);
    themeBtn->setObjectName(QLatin1String("aroraReaderTheme"));
    themeBtn->setText(dark ? QStringLiteral("\u2600")
                           : QStringLiteral("\u263E"));
    themeBtn->setToolTip(tr("Switch between light and dark"));
    QToolButton *exitBtn = new QToolButton(bar);
    exitBtn->setObjectName(QLatin1String("aroraReaderExit"));
    exitBtn->setText(tr("Exit reader view"));
    barLayout->addWidget(tag);
    barLayout->addWidget(fontDec);
    barLayout->addWidget(fontInc);
    barLayout->addWidget(themeBtn);
    barLayout->addStretch(1);
    barLayout->addWidget(exitBtn);
    layout->addWidget(bar);

    // The article surface — a dedicated view on the page's own
    // profile, so images and links resolve through the same proxy,
    // cookie and interceptor context the page has.
    QWebEngineView *readerView = new QWebEngineView(overlay);
    readerView->setObjectName(QLatin1String("aroraReaderView"));
    readerView->setPage(new ReaderOverlayPage(
        page->profile(), baseUrl, m_view, readerView));
    readerView->setHtml(readerDocument(article, startTheme), baseUrl);
    readerView->setZoomFactor(fontSize() / 19.0);
    layout->addWidget(readerView, 1);

    connect(fontDec, &QToolButton::clicked, this,
            [this, readerView]() {
        const int px = qBound(12, fontSize() - 1, 30);
        QSettings().setValue(QLatin1String("reader/fontSize"), px);
        readerView->setZoomFactor(px / 19.0);
    });
    connect(fontInc, &QToolButton::clicked, this,
            [this, readerView]() {
        const int px = qBound(12, fontSize() + 1, 30);
        QSettings().setValue(QLatin1String("reader/fontSize"), px);
        readerView->setZoomFactor(px / 19.0);
    });
    connect(themeBtn, &QToolButton::clicked, this,
            [this, bar, readerView, themeBtn]() {
        const QString next = theme() == QLatin1String("dark")
            ? QStringLiteral("light") : QStringLiteral("dark");
        QSettings().setValue(QLatin1String("reader/theme"), next);
        const bool darkNext = next == QLatin1String("dark");
        themeBtn->setText(darkNext ? QStringLiteral("\u2600")
                                   : QStringLiteral("\u263E"));
        bar->setStyleSheet(QStringLiteral(
            "#aroraReaderBar{background:%1;color:%2}"
            "#aroraReaderBar QLabel{font:13px system-ui;opacity:.7}"
            "#aroraReaderBar QToolButton{font:13px system-ui;color:%2;"
            "background:transparent;border:1px solid transparent;"
            "border-radius:4px;padding:3px 8px}"
            "#aroraReaderBar QToolButton:hover{"
            "background:rgba(127,127,127,.18);"
            "border-color:rgba(127,127,127,.35)}")
            .arg(darkNext ? QLatin1String("#262628")
                          : QLatin1String("#efece4"),
                 darkNext ? QLatin1String("#b9b4aa")
                          : QLatin1String("#4a463d")));
        // The surface's own document — live-apply the theme class
        // without a reload (article scroll position survives).
        readerView->page()->runJavaScript(QStringLiteral(
            "document.body.className='theme-%1';").arg(next));
    });
    connect(exitBtn, &QToolButton::clicked, this, &ReaderMode::exit);

    overlay->setGeometry(m_view->rect());
    overlay->show();
    overlay->raise();
    m_overlay = overlay;
}

void ReaderMode::closeOverlay()
{
    if (!m_overlay)
        return;
    QWidget *overlay = m_overlay;
    m_overlay = nullptr;
    overlay->hide();
    overlay->deleteLater();
}
#endif // ARORA_RUSTCORE

void ReaderMode::enter()
{
    QWebEnginePage *page = m_view ? m_view->page() : nullptr;
    if (!page || m_busy)
        return;
    if (m_active) {
        // Belt-and-braces for a stale flag: verify the overlay is
        // actually there before declaring victory.
        setActive(false);
    }

#ifdef ARORA_RUSTCORE
    {
        const QUrl pageUrl = page->url();
        m_busy = true;
        QPointer<ReaderMode> self(this);
        collectHtml([self, pageUrl](const QString &html) {
            if (!self)
                return;
            self->m_busy = false;
            const QByteArray utf8 = html.toUtf8();
            const QByteArray url8 = pageUrl.toString().toUtf8();
            RcBuffer out { nullptr, 0 };
            const RcStatus status = rc_readability_extract(
                reinterpret_cast<const uint8_t *>(utf8.constData()),
                static_cast<size_t>(utf8.size()), url8.constData(), &out);
            const QByteArray json(
                reinterpret_cast<const char *>(out.data),
                static_cast<qsizetype>(out.len));
            if (out.data)
                rc_buffer_free(out);
            const QVariantMap map = QJsonDocument::fromJson(json)
                .object().toVariantMap();
            if (status == RC_OK
                && map.value(QLatin1String("ok")).toBool()) {
                self->showOverlay(map);
                self->setActive(true);
            } else {
                emit self->message(
                    tr("Reader view is not available on this page."));
            }
        });
        return;
    }
#endif

    QJsonObject labels;
    labels[QLatin1String("tag")] = tr("Reader");
    labels[QLatin1String("fontDec")] = tr("Smaller text");
    labels[QLatin1String("fontInc")] = tr("Larger text");
    labels[QLatin1String("theme")] = tr("Switch between light and dark");
    labels[QLatin1String("exit")] = tr("Exit reader view");
    QJsonObject opts;
    opts[QLatin1String("fontSize")] = fontSize();
    opts[QLatin1String("theme")] = theme();
    opts[QLatin1String("labels")] = labels;
    const QString call = QLatin1String("window.__aroraReader.enter(")
        + QString::fromUtf8(
            QJsonDocument(opts).toJson(QJsonDocument::Compact))
        + QLatin1String(");");

    m_busy = true;
    QPointer<ReaderMode> self(this);
    runDriver(call, [self](const QVariant &result) {
        if (!self)
            return;
        self->m_busy = false;
        const QVariantMap map = result.toMap();
        if (map.value(QLatin1String("ok")).toBool()) {
            self->setActive(true);
        } else {
            emit self->message(
                tr("Reader view is not available on this page."));
        }
    });
}

void ReaderMode::exit()
{
    if (!m_active)
        return;
#ifdef ARORA_RUSTCORE
    setActive(false);
    return;
#endif
    QPointer<ReaderMode> self(this);
    runDriver(QLatin1String("window.__aroraReader.exit();"),
            [self](const QVariant &) {
        if (self)
            self->setActive(false);
    });
    // Do not wait on the callback: on a tor page there is no bridge
    // and a wedged renderer would leave the check state stuck.
    setActive(false);
}

void ReaderMode::toggle()
{
    if (m_active)
        exit();
    else
        enter();
}
