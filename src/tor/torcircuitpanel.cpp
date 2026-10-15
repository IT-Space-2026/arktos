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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "torcircuitpanel.h"

#include "arorastyle.h"
#include "torcircuitbutton.h"

#include <qboxlayout.h>
#include <qfontmetrics.h>
#include <qlabel.h>
#include <qpainter.h>
#include <qpushbutton.h>

namespace {
// Chain-view layout metrics — the left gutter carries the painted
// node dots on a continuous vertical connector; flags sit between
// the gutter and the text column like the reference screenshot.
const int kDotX = 10;        // node-dot center x
const int kDotR = 4;         // node-dot radius
const int kFlagX = 24;
const int kFlagSize = 16;
const int kTextX = 46;       // all node text left edge
const int kVGap = 7;         // space between node blocks
const int kPadY = 4;
const int kMaxTextWidth = 340;
}

// The painted vertical chain — one block per node: a dot on the
// connector line, an optional flag glyph, a title line and smaller
// detail lines underneath.
class TorChainView : public QWidget
{
public:
    explicit TorChainView(QWidget *parent = nullptr)
        : QWidget(parent)
    {
    }

    void setNodes(const QList<TorCircuitPanel::Node> &nodes)
    {
        m_nodes = nodes;
        updateGeometry();
        update();
    }

    QSize sizeHint() const override
    {
        const QFontMetrics titleFm(font());
        const QFontMetrics detailFm(detailFont());
        int width = kTextX + 280;
        int height = 2 * kPadY;
        for (const TorCircuitPanel::Node &node : m_nodes) {
            width = qMax(width, kTextX + qMin(
                    titleFm.horizontalAdvance(node.title),
                    kMaxTextWidth));
            for (const QString &detail : node.details)
                width = qMax(width, kTextX + qMin(
                        detailFm.horizontalAdvance(detail),
                        kMaxTextWidth));
            height += blockHeight(node);
        }
        if (!m_nodes.isEmpty())
            height -= kVGap;
        // TOR16: never size below the standard 3-hop chain (browser
        // + three hop rows + site).  The popup takes its size on
        // open and a shown menu does not re-lay out, so reserving
        // the full chain keeps the rendered size stable whether the
        // circuit snapshot lands before or after the click — the
        // "sometimes big enough, sometimes not" the user reported
        // was the popup squeezing a late-arriving chain into the
        // info-state footprint.
        const int titleH = titleFm.height();
        const int detailH = detailFm.height();
        int reserve = 2 * kPadY;
        reserve += titleH + kVGap;                    // "This browser"
        reserve += 3 * (titleH + 2 * detailH + kVGap); // three hops
        reserve += titleH;                            // the site
        return QSize(width, qMax(height, reserve));
    }

    QSize minimumSizeHint() const override
    {
        return sizeHint();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.setRenderHint(QPainter::Antialiasing);
        const QFontMetrics titleFm(font());
        const QFont detail = detailFont();
        const QFontMetrics detailFm(detail);

        // First pass: node-dot centers, so the connector line runs
        // unbroken from the first node to the last.
        QList<int> centers;
        centers.reserve(m_nodes.size());
        int y = kPadY;
        for (const TorCircuitPanel::Node &node : m_nodes) {
            centers << y + titleFm.height() / 2;
            y += blockHeight(node);
        }
        if (centers.size() >= 2) {
            // TOR16: Mid can sit on (or equal) the window color on
            // flat/high-contrast themes, which made the connector
            // invisible — the user saw disconnected flag glyphs.
            // A translucent WindowText line contrasts its own
            // background on every palette.
            QColor connector = palette().color(QPalette::WindowText);
            connector.setAlpha(110);
            painter.setPen(QPen(connector, 2));
            painter.drawLine(kDotX, centers.first(),
                             kDotX, centers.last());
        }

        const QColor dotColor = palette().color(QPalette::Highlight);
        const QColor titleColor =
            palette().color(QPalette::WindowText);
        const QColor detailColor =
            palette().color(QPalette::Disabled, QPalette::WindowText);
        const qreal dpr = devicePixelRatioF();
        // The text column's real width — clamp so a squeezed widget
        // never hands drawText an empty/negative rect (which hides
        // every line while the fixed-position flags still paint —
        // the user's "only the flags show" symptom).
        const int textW = qMin(qMax(0, width() - kTextX), kMaxTextWidth);
        y = kPadY;
        for (int i = 0; i < m_nodes.size(); ++i) {
            const TorCircuitPanel::Node &node = m_nodes.at(i);
            const int titleH = titleFm.height();
            painter.setPen(Qt::NoPen);
            painter.setBrush(dotColor);
            painter.drawEllipse(QPointF(kDotX, centers.at(i)),
                                kDotR, kDotR);
            painter.setBrush(Qt::NoBrush);
            if (!node.countryCode.isEmpty()) {
                TorCircuitButton::flagIcon(node.countryCode, dpr)
                    .paint(&painter,
                           QRect(kFlagX,
                                 y + (titleH - kFlagSize) / 2,
                                 kFlagSize, kFlagSize));
            }
            painter.setFont(font());
            painter.setPen(titleColor);
            painter.drawText(
                QRect(kTextX, y, textW, titleH),
                int(Qt::AlignLeft | Qt::AlignVCenter),
                titleFm.elidedText(node.title, Qt::ElideRight,
                                   textW));
            y += titleH;
            painter.setFont(detail);
            painter.setPen(detailColor);
            for (const QString &line : node.details) {
                painter.drawText(
                    QRect(kTextX, y, textW,
                          detailFm.height()),
                    int(Qt::AlignLeft | Qt::AlignVCenter),
                    detailFm.elidedText(line, Qt::ElideRight,
                                        textW));
                y += detailFm.height();
            }
            y += kVGap;
        }
        painter.end();
    }

private:
    QFont detailFont() const
    {
        QFont detail = font();
        if (detail.pointSizeF() > 0)
            detail.setPointSizeF(qMax<qreal>(7.0,
                                             detail.pointSizeF() - 1));
        else
            detail.setPixelSize(qMax(9, detail.pixelSize() - 1));
        return detail;
    }

    int blockHeight(const TorCircuitPanel::Node &node) const
    {
        return QFontMetrics(font()).height()
               + node.details.size()
                     * QFontMetrics(detailFont()).height()
               + kVGap;
    }

    QList<TorCircuitPanel::Node> m_nodes;
};

TorCircuitPanel::TorCircuitPanel(QWidget *parent)
    : QFrame(parent)
    , m_showingCircuit(false)
{
    setObjectName(QLatin1String("torCircuitPanel"));
    setAccessibleName(tr("Tor circuit"));
    setAccessibleDescription(
        tr("The relays in the current Tor circuit."));

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(AroraStyle::Spacing::Large,
                               AroraStyle::Spacing::Medium,
                               AroraStyle::Spacing::Large,
                               AroraStyle::Spacing::Medium);
    layout->setSpacing(AroraStyle::Spacing::Medium);

    m_title = new QLabel(this);
    QFont titleFont = m_title->font();
    titleFont.setBold(true);
    m_title->setFont(titleFont);
    layout->addWidget(m_title);

    m_chain = new TorChainView(this);
    m_chain->setObjectName(QLatin1String("torChainView"));
    layout->addWidget(m_chain);

    m_info = new QLabel(this);
    m_info->setWordWrap(true);
    layout->addWidget(m_info);

    m_newCircuit = new QPushButton(
        tr("New Tor circuit for this site"), this);
    m_newCircuit->setObjectName(QLatin1String("newCircuitButton"));
    // TOR16: a taller target — the bare-hint button read as a cramped
    // text row; Tor Browser's equivalent is a prominent full-width
    // action.
    m_newCircuit->setMinimumHeight(30);
    connect(m_newCircuit, &QPushButton::clicked,
            this, &TorCircuitPanel::newCircuitRequested);
    layout->addWidget(m_newCircuit);

    m_status = new QLabel(this);
    m_status->setWordWrap(true);
    layout->addWidget(m_status);

    m_note = new QLabel(tr("Your guard node may not change."), this);
    m_note->setForegroundRole(QPalette::PlaceholderText);
    m_note->setWordWrap(true);
    layout->addWidget(m_note);

    m_info->hide();
    m_status->hide();
}

void TorCircuitPanel::setCircuit(const QString &siteHost,
                                 const QList<Node> &nodes)
{
    m_title->setText(siteHost.isEmpty()
        ? tr("Circuit for this site")
        : tr("Circuit for %1").arg(siteHost));
    m_nodes = nodes;
    m_infoText.clear();
    m_showingCircuit = true;
    m_chain->setNodes(nodes);
    m_chain->show();
    m_info->hide();
    updateGeometry();
}

void TorCircuitPanel::setInfo(const QString &siteHost,
                              const QString &text)
{
    m_title->setText(siteHost.isEmpty()
        ? tr("Circuit for this site")
        : tr("Circuit for %1").arg(siteHost));
    m_nodes.clear();
    m_infoText = text;
    m_showingCircuit = false;
    m_info->setText(text);
    m_info->show();
    m_chain->hide();
    updateGeometry();
}

void TorCircuitPanel::setNewCircuitEnabled(bool enabled,
                                             const QString &disabledReason)
{
    m_newCircuit->setEnabled(enabled);
    m_newCircuit->setToolTip(enabled || disabledReason.isEmpty()
        ? tr("Ask tor for a fresh circuit; this site is reloaded "
             "onto it.")
        : disabledReason);
}

void TorCircuitPanel::setStatusText(const QString &text)
{
    m_status->setText(text);
    m_status->setVisible(!text.isEmpty());
}

QString TorCircuitPanel::titleText() const
{
    return m_title->text();
}

QStringList TorCircuitPanel::contentLines() const
{
    QStringList lines;
    lines << m_title->text();
    if (m_showingCircuit) {
        for (const Node &node : m_nodes) {
            lines << node.title;
            lines << node.details;
        }
    } else {
        lines << m_infoText;
    }
    lines << m_newCircuit->text();
    if (!m_status->text().isEmpty())
        lines << m_status->text();
    lines << m_note->text();
    return lines;
}

int TorCircuitPanel::hopCount() const
{
    int count = 0;
    for (const Node &node : m_nodes) {
        if (node.kind == Node::Hop)
            ++count;
    }
    return count;
}

QString TorCircuitPanel::roleName(int index, int count)
{
    if (count == 3)
        return index == 0 ? tr("guard")
             : index == count - 1 ? tr("exit")
             : tr("middle");
    return tr("hop %1").arg(index + 1);
}

QString TorCircuitPanel::shortFingerprint(const QString &fingerprint)
{
    if (fingerprint.size() <= 20)
        return fingerprint;
    return fingerprint.left(8) + QStringLiteral("…")
           + fingerprint.right(8);
}

TorCircuitPanel::Node TorCircuitPanel::hopNode(const TorCircuitHop &hop,
                                               int index, int count)
{
    Node node;
    node.kind = Node::Hop;
    node.countryCode = hop.country;
    QString country;
    if (!hop.country.isEmpty()) {
        country = TorCircuitButton::countryName(hop.country);
        if (country.isEmpty())
            country = hop.country;
    } else {
        country = tr("Unknown");
    }
    node.title = tr("%1 (%2)").arg(country, roleName(index, count));
    QString location = hop.address;
    if (!hop.nickname.isEmpty()) {
        if (!location.isEmpty())
            location += QStringLiteral(" — ");
        location += hop.nickname;
    }
    if (!location.isEmpty())
        node.details << location;
    const QString fingerprint = shortFingerprint(hop.fingerprint);
    if (!fingerprint.isEmpty())
        node.details << fingerprint;
    return node;
}

QList<TorCircuitPanel::Node> TorCircuitPanel::nodesForCircuit(
        const TorCircuit &circuit, const QString &siteHost)
{
    QList<Node> nodes;
    Node browser;
    browser.kind = Node::Browser;
    browser.title = tr("This browser");
    nodes << browser;
    for (int i = 0; i < circuit.hops.size(); ++i)
        nodes << hopNode(circuit.hops.at(i), i, circuit.hops.size());
    Node site;
    site.kind = Node::Site;
    site.title = siteHost.isEmpty() ? tr("this site") : siteHost;
    nodes << site;
    return nodes;
}
