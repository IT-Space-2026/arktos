/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "locationbar.h"

#include "clearbutton.h"
#include "locationbarsiteicon.h"
#include "popupblockerbutton.h"
#include "privacyindicator.h"
#include "readerbutton.h"
#include "searchlineedit.h"
#include "siteshield.h"
#include "twoleveldomains_p.h"
#include "webview.h"

#include <qapplication.h>
#include <qdrag.h>
#include <qevent.h>
#include <qhostaddress.h>
#include <qmimedata.h>
#include <qpainter.h>
#include <qstyleoption.h>
#include <qtooltip.h>

#include <qdebug.h>

LocationBar::LocationBar(QWidget *parent)
    : LineEdit(parent)
    , m_webView(nullptr)
    , m_siteIcon(nullptr)
    , m_shield(nullptr)
    , m_popupBlockerButton(nullptr)
    , m_privacyIndicator(nullptr)
    , m_readerButton(nullptr)
{
    // Urls are always LeftToRight
    setLayoutDirection(Qt::LeftToRight);
    setAccessibleName(tr("Address Bar"));

    // UIP01: modern omnibox height — the Qt4 default single-line
    // height reads cramped next to the padded navigation toolbar.
    setMinimumHeight(fontMetrics().height() + 12);

    setUpdatesEnabled(false);
    // UIP05: the shield indicator opens the per-site privacy panel —
    // leftmost of the leading site-info zone, Vivaldi-style.
    m_shield = new SiteShieldButton(this);
    addWidget(m_shield, LeftSide);

    // site icon on the left, next to the shield
    m_siteIcon = new LocationBarSiteIcon(this);
    addWidget(m_siteIcon, LeftSide);

    // POPUP01: blocked pop-up indicator — rightmost of the right-side
    // cluster (widgets are inserted at index 1, so the first added
    // lands furthest right); appears only while the page has refused
    // pop-ups.  SHLD02: the content-blocker button that used to anchor
    // this cluster was merged into the left shield.
    m_popupBlockerButton = new PopupBlockerButton(this);
    addWidget(m_popupBlockerButton, RightSide);

    // privacy indicator at rightmost position
    m_privacyIndicator = new PrivacyIndicator(this);
    addWidget(m_privacyIndicator, RightSide);

    // READ01: reader-mode toggle — appears on article-like pages,
    // left of the privacy indicator (widgets land at index 1).
    m_readerButton = new ReaderButton(this);
    addWidget(m_readerButton, RightSide);

    // clear button on the right
    ClearButton *m_clearButton = new ClearButton(this);
    connect(m_clearButton, &ClearButton::clicked,
            this, &QLineEdit::clear);
    connect(this, &QLineEdit::textChanged,
            m_clearButton, &ClearButton::textChanged);
    addWidget(m_clearButton, RightSide);

    updateTextMargins();
    setUpdatesEnabled(true);
}

void LocationBar::setWebView(WebView *webView)
{
    Q_ASSERT(webView);
    m_webView = webView;
    m_siteIcon->setWebView(webView);
    m_shield->setWebView(webView);
    m_popupBlockerButton->setWebView(webView);
    m_privacyIndicator->setWebView(webView);
    m_readerButton->setWebView(webView);
    connect(webView, &QWebEngineView::urlChanged,
            this, &LocationBar::webViewUrlChanged);
    connect(webView, &QWebEngineView::loadProgress,
            this, [this]() { update(); });
}

WebView *LocationBar::webView() const
{
    return m_webView;
}

void LocationBar::webViewUrlChanged(const QUrl &url)
{
    if (hasFocus())
        return;
    displayUrl(url);
}

void LocationBar::displayUrl(const QUrl &url)
{
    setText(QString::fromUtf8(url.toEncoded()));
    setCursorPosition(0);
}

bool LocationBar::registrableDomainRange(const QString &displayText,
                                         int &start, int &length)
{
    start = -1;
    length = 0;

    // <scheme>://<authority><path…> — no authority, nothing to mark.
    const int schemeEnd = displayText.indexOf(QLatin1String("://"));
    if (schemeEnd < 0)
        return false;
    const int authorityStart = schemeEnd + 3;
    int authorityEnd = displayText.size();
    for (int i = authorityStart; i < displayText.size(); ++i) {
        const QChar c = displayText.at(i);
        if (c == QLatin1Char('/') || c == QLatin1Char('?')
            || c == QLatin1Char('#')) {
            authorityEnd = i;
            break;
        }
    }

    // userinfo@ — the host starts after the last '@' in the authority.
    int hostStart = authorityStart;
    const int at = displayText.lastIndexOf(QLatin1Char('@'),
                                           authorityEnd - 1);
    if (at >= authorityStart)
        hostStart = at + 1;
    if (hostStart >= authorityEnd)
        return false;

    if (displayText.at(hostStart) == QLatin1Char('[')) {
        // IPv6/IPvFuture literal: emphasize the bracketed host whole.
        const int hostEnd =
            displayText.indexOf(QLatin1Char(']'), hostStart + 1);
        if (hostEnd < 0 || hostEnd >= authorityEnd)
            return false;
        start = hostStart;
        length = hostEnd - hostStart + 1;
        return true;
    }

    // :port ends the host.
    int hostEnd = authorityEnd;
    const int colon = displayText.indexOf(QLatin1Char(':'), hostStart);
    if (colon >= 0 && colon < authorityEnd)
        hostEnd = colon;

    // FQDN root dot(s) belong to no label.
    int end = hostEnd;
    while (end > hostStart
           && displayText.at(end - 1) == QLatin1Char('.'))
        --end;
    if (end <= hostStart)
        return false;

    // IP literals have no registrable domain — emphasize the address.
    QHostAddress address;
    if (address.setAddress(
            QStringView(displayText).mid(hostStart, end - hostStart)
                .toString())) {
        start = hostStart;
        length = end - hostStart;
        return true;
    }

    const QStringView view(displayText);
    const int lastDot =
        displayText.lastIndexOf(QLatin1Char('.'), end - 1);
    const QStringView tld = lastDot < hostStart
        ? view.mid(hostStart, end - hostStart)
        : view.mid(lastDot + 1, end - lastDot - 1);

    bool twoLevel = false;
    for (int i = 0; twoLevelDomains[i]; ++i) {
        if (!tld.compare(QLatin1String(twoLevelDomains[i]),
                         Qt::CaseInsensitive)) {
            twoLevel = true;
            break;
        }
    }
    const int needed = twoLevel ? 3 : 2;

    // Walk the dots backwards; each one delimits one more label.
    int pos = end;
    int found = 0;
    int domainStart = hostStart;
    while (pos > hostStart && found < needed) {
        const int dot = displayText.lastIndexOf(QLatin1Char('.'),
                                                pos - 1);
        if (dot < hostStart)
            break;
        domainStart = dot + 1;
        ++found;
        pos = dot;
    }
    if (found < needed)
        domainStart = hostStart;

    start = domainStart;
    length = end - domainStart;
    return true;
}

QString LocationBar::unicodeUrlHint(const QString &displayText)
{
    const QUrl url = QUrl::fromEncoded(displayText.toUtf8());
    const QString host = url.host();
    if (host.isEmpty() || host == url.host(QUrl::FullyEncoded))
        return QString();
    return url.toString(QUrl::RemovePassword);
}

bool LocationBar::event(QEvent *event)
{
    // The bar stores the url's encoded form, so an internationalized
    // host already renders as xn-- punycode; offer the Unicode form as
    // a tooltip (Chrome-style) whenever it differs.
    if (event->type() == QEvent::ToolTip) {
        const QString hint = unicodeUrlHint(text());
        if (!hint.isEmpty()) {
            QHelpEvent *help = static_cast<QHelpEvent*>(event);
            QToolTip::showText(help->globalPos(),
                               tr("Unicode form: %1").arg(hint), this);
            return true;
        }
    }
    return LineEdit::event(event);
}

void LocationBar::paintEvent(QPaintEvent *event)
{
    QPalette p = palette();
    QColor defaultBaseColor = QApplication::palette().color(QPalette::Base);
    QColor backgroundColor = defaultBaseColor;
    if (m_webView && m_webView->url().scheme() == QLatin1String("https")
        && p.color(QPalette::Text).value() < 128) {
        QColor lightYellow(248, 248, 210);
        backgroundColor = lightYellow;
    }

    // set the progress bar
    QBrush baseBrush = QBrush(backgroundColor);
    if (m_webView) {
        int progress = m_webView->progress();
        if (progress == 0) {
            p.setBrush(QPalette::Base, backgroundColor);
        } else {
            QColor loadingColor = QColor(116, 192, 250);
            if (p.color(QPalette::Text).value() >= 128)
                loadingColor = defaultBaseColor.darker(200);

            QLinearGradient gradient(0, 0, width(), 0);
            gradient.setColorAt(0, loadingColor);
            gradient.setColorAt(((double)progress)/100, backgroundColor);
            p.setBrush(QPalette::Base, gradient);
            baseBrush = gradient;
        }
        setPalette(p);
    }

    LineEdit::paintEvent(event);

    // SAFE03: with the url on display (not being edited), repaint the
    // text dimmed except the registrable domain so the site's real
    // identity stands out — the paypal.com.evil.tld trick can't hide
    // the domain inside the subdomain run.  cursorPosition()==0 keeps
    // the scroll offset at zero so the segment maths below line up
    // with where QLineEdit painted the text.
    int start = -1;
    int length = 0;
    if (hasFocus() || hasSelectedText() || cursorPosition() != 0
        || !registrableDomainRange(text(), start, length))
        return;

    QStyleOptionFrame panel;
    initStyleOption(&panel);
    QRect textRect =
        style()->subElementRect(QStyle::SE_LineEditContents, &panel, this);
    // same text area LineEdit::paintEvent uses for inactiveText.
    textRect.adjust(2, 0, -2, 0);
    textRect.adjust(textMargin(LineEdit::LeftSide), 0,
                    -textMargin(LineEdit::RightSide), 0);
    if (textRect.isEmpty())
        return;

    const QString shown = text();
    const QFontMetrics fm = fontMetrics();
    const int domainX =
        textRect.x() + fm.horizontalAdvance(shown.left(start));
    const int domainWidth =
        fm.horizontalAdvance(shown.mid(start, length));
    const int baseline =
        textRect.y() + (textRect.height() - fm.height() + 1) / 2
        + fm.ascent();

    QPainter painter(this);
    painter.setClipRect(textRect);
    painter.fillRect(textRect, baseBrush);
    const QColor faded =
        palette().color(QPalette::Disabled, QPalette::Text);
    painter.setPen(faded);
    painter.drawText(textRect.x(), baseline, shown.left(start));
    painter.drawText(domainX + domainWidth, baseline,
                     shown.mid(start + length));
    painter.setPen(palette().color(QPalette::Text));
    painter.drawText(domainX, baseline, shown.mid(start, length));
}

void LocationBar::focusOutEvent(QFocusEvent *event)
{
    if (text().isEmpty() && m_webView)
        webViewUrlChanged(m_webView->url());
    // Home the scroll so the SAFE03 domain-emphasis repaint (which
    // assumes an unscrolled bar) lines up on an unfocused widget.
    setCursorPosition(0);
    QLineEdit::focusOutEvent(event);
}

void LocationBar::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton)
        selectAll();
    else
        QLineEdit::mouseDoubleClickEvent(event);
}

void LocationBar::keyPressEvent(QKeyEvent *event)
{
    if (event->key() == Qt::Key_Escape && m_webView) {
        displayUrl(m_webView->url());
        selectAll();
        return;
    }

    QString currentText = text().trimmed();
    if ((event->key() == Qt::Key_Enter || event->key() == Qt::Key_Return)
        && !currentText.startsWith(QLatin1String("http://"), Qt::CaseInsensitive)) {
        QString append;
        if (event->modifiers() == Qt::ControlModifier)
            append = QLatin1String(".com");
        else if (event->modifiers() == (Qt::ControlModifier | Qt::ShiftModifier))
            append = QLatin1String(".org");
        else if (event->modifiers() == Qt::ShiftModifier)
            append = QLatin1String(".net");
        QUrl url(QLatin1String("http://") + currentText);
        QString host = url.host();
        if (!host.endsWith(append, Qt::CaseInsensitive)) {
            host += append;
            url.setHost(host);
            setText(url.toString());
        }
    }

    LineEdit::keyPressEvent(event);
}

void LocationBar::dragEnterEvent(QDragEnterEvent *event)
{
    const QMimeData *mimeData = event->mimeData();
    if (mimeData->hasUrls() || mimeData->hasText())
        event->acceptProposedAction();

    LineEdit::dragEnterEvent(event);
}

void LocationBar::dropEvent(QDropEvent *event)
{
    const QMimeData *mimeData = event->mimeData();

    QUrl url;
    if (mimeData->hasUrls())
        url = mimeData->urls().at(0);
    else if (mimeData->hasText())
        url = QUrl::fromEncoded(mimeData->text().toUtf8(), QUrl::TolerantMode);

    if (url.isEmpty() || !url.isValid()) {
        LineEdit::dropEvent(event);
        return;
    }

    displayUrl(url);
    selectAll();

    event->acceptProposedAction();
}
