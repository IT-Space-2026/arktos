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

#include "torcircuitbutton.h"

#include <qaction.h>
#include <qevent.h>
#include <qmenu.h>
#include <qpainter.h>
#include <qpainterpath.h>

// ISO-3166-1 alpha-2 -> English name, sorted by code for binary
// search.  tor's geoip only ever reports codes, so the display name
// has to come from a bundled table.
static const char *const kCountryNames[][2] = {
    { "AD", "Andorra" },
    { "AE", "United Arab Emirates" },
    { "AF", "Afghanistan" },
    { "AG", "Antigua and Barbuda" },
    { "AI", "Anguilla" },
    { "AL", "Albania" },
    { "AM", "Armenia" },
    { "AO", "Angola" },
    { "AQ", "Antarctica" },
    { "AR", "Argentina" },
    { "AS", "American Samoa" },
    { "AT", "Austria" },
    { "AU", "Australia" },
    { "AW", "Aruba" },
    { "AX", "Åland Islands" },
    { "AZ", "Azerbaijan" },
    { "BA", "Bosnia and Herzegovina" },
    { "BB", "Barbados" },
    { "BD", "Bangladesh" },
    { "BE", "Belgium" },
    { "BF", "Burkina Faso" },
    { "BG", "Bulgaria" },
    { "BH", "Bahrain" },
    { "BI", "Burundi" },
    { "BJ", "Benin" },
    { "BL", "Saint Barthélemy" },
    { "BM", "Bermuda" },
    { "BN", "Brunei" },
    { "BO", "Bolivia" },
    { "BQ", "Caribbean Netherlands" },
    { "BR", "Brazil" },
    { "BS", "Bahamas" },
    { "BT", "Bhutan" },
    { "BV", "Bouvet Island" },
    { "BW", "Botswana" },
    { "BY", "Belarus" },
    { "BZ", "Belize" },
    { "CA", "Canada" },
    { "CC", "Cocos (Keeling) Islands" },
    { "CD", "DR Congo" },
    { "CF", "Central African Republic" },
    { "CG", "Republic of the Congo" },
    { "CH", "Switzerland" },
    { "CI", "Côte d'Ivoire" },
    { "CK", "Cook Islands" },
    { "CL", "Chile" },
    { "CM", "Cameroon" },
    { "CN", "China" },
    { "CO", "Colombia" },
    { "CR", "Costa Rica" },
    { "CU", "Cuba" },
    { "CV", "Cape Verde" },
    { "CW", "Curaçao" },
    { "CX", "Christmas Island" },
    { "CY", "Cyprus" },
    { "CZ", "Czechia" },
    { "DE", "Germany" },
    { "DJ", "Djibouti" },
    { "DK", "Denmark" },
    { "DM", "Dominica" },
    { "DO", "Dominican Republic" },
    { "DZ", "Algeria" },
    { "EC", "Ecuador" },
    { "EE", "Estonia" },
    { "EG", "Egypt" },
    { "EH", "Western Sahara" },
    { "ER", "Eritrea" },
    { "ES", "Spain" },
    { "ET", "Ethiopia" },
    { "FI", "Finland" },
    { "FJ", "Fiji" },
    { "FK", "Falkland Islands" },
    { "FM", "Micronesia" },
    { "FO", "Faroe Islands" },
    { "FR", "France" },
    { "GA", "Gabon" },
    { "GB", "United Kingdom" },
    { "GD", "Grenada" },
    { "GE", "Georgia" },
    { "GF", "French Guiana" },
    { "GG", "Guernsey" },
    { "GH", "Ghana" },
    { "GI", "Gibraltar" },
    { "GL", "Greenland" },
    { "GM", "Gambia" },
    { "GN", "Guinea" },
    { "GP", "Guadeloupe" },
    { "GQ", "Equatorial Guinea" },
    { "GR", "Greece" },
    { "GS", "South Georgia and the South Sandwich Islands" },
    { "GT", "Guatemala" },
    { "GU", "Guam" },
    { "GW", "Guinea-Bissau" },
    { "GY", "Guyana" },
    { "HK", "Hong Kong" },
    { "HM", "Heard Island and McDonald Islands" },
    { "HN", "Honduras" },
    { "HR", "Croatia" },
    { "HT", "Haiti" },
    { "HU", "Hungary" },
    { "ID", "Indonesia" },
    { "IE", "Ireland" },
    { "IL", "Israel" },
    { "IM", "Isle of Man" },
    { "IN", "India" },
    { "IO", "British Indian Ocean Territory" },
    { "IQ", "Iraq" },
    { "IR", "Iran" },
    { "IS", "Iceland" },
    { "IT", "Italy" },
    { "JE", "Jersey" },
    { "JM", "Jamaica" },
    { "JO", "Jordan" },
    { "JP", "Japan" },
    { "KE", "Kenya" },
    { "KG", "Kyrgyzstan" },
    { "KH", "Cambodia" },
    { "KI", "Kiribati" },
    { "KM", "Comoros" },
    { "KN", "Saint Kitts and Nevis" },
    { "KP", "North Korea" },
    { "KR", "South Korea" },
    { "KW", "Kuwait" },
    { "KY", "Cayman Islands" },
    { "KZ", "Kazakhstan" },
    { "LA", "Laos" },
    { "LB", "Lebanon" },
    { "LC", "Saint Lucia" },
    { "LI", "Liechtenstein" },
    { "LK", "Sri Lanka" },
    { "LR", "Liberia" },
    { "LS", "Lesotho" },
    { "LT", "Lithuania" },
    { "LU", "Luxembourg" },
    { "LV", "Latvia" },
    { "LY", "Libya" },
    { "MA", "Morocco" },
    { "MC", "Monaco" },
    { "MD", "Moldova" },
    { "ME", "Montenegro" },
    { "MF", "Saint Martin" },
    { "MG", "Madagascar" },
    { "MH", "Marshall Islands" },
    { "MK", "North Macedonia" },
    { "ML", "Mali" },
    { "MM", "Myanmar" },
    { "MN", "Mongolia" },
    { "MO", "Macao" },
    { "MP", "Northern Mariana Islands" },
    { "MQ", "Martinique" },
    { "MR", "Mauritania" },
    { "MS", "Montserrat" },
    { "MT", "Malta" },
    { "MU", "Mauritius" },
    { "MV", "Maldives" },
    { "MW", "Malawi" },
    { "MX", "Mexico" },
    { "MY", "Malaysia" },
    { "MZ", "Mozambique" },
    { "NA", "Namibia" },
    { "NC", "New Caledonia" },
    { "NE", "Niger" },
    { "NF", "Norfolk Island" },
    { "NG", "Nigeria" },
    { "NI", "Nicaragua" },
    { "NL", "Netherlands" },
    { "NO", "Norway" },
    { "NP", "Nepal" },
    { "NR", "Nauru" },
    { "NU", "Niue" },
    { "NZ", "New Zealand" },
    { "OM", "Oman" },
    { "PA", "Panama" },
    { "PE", "Peru" },
    { "PF", "French Polynesia" },
    { "PG", "Papua New Guinea" },
    { "PH", "Philippines" },
    { "PK", "Pakistan" },
    { "PL", "Poland" },
    { "PM", "Saint Pierre and Miquelon" },
    { "PN", "Pitcairn" },
    { "PR", "Puerto Rico" },
    { "PS", "Palestine" },
    { "PT", "Portugal" },
    { "PW", "Palau" },
    { "PY", "Paraguay" },
    { "QA", "Qatar" },
    { "RE", "Réunion" },
    { "RO", "Romania" },
    { "RS", "Serbia" },
    { "RU", "Russia" },
    { "RW", "Rwanda" },
    { "SA", "Saudi Arabia" },
    { "SB", "Solomon Islands" },
    { "SC", "Seychelles" },
    { "SD", "Sudan" },
    { "SE", "Sweden" },
    { "SG", "Singapore" },
    { "SH", "Saint Helena" },
    { "SI", "Slovenia" },
    { "SJ", "Svalbard and Jan Mayen" },
    { "SK", "Slovakia" },
    { "SL", "Sierra Leone" },
    { "SM", "San Marino" },
    { "SN", "Senegal" },
    { "SO", "Somalia" },
    { "SR", "Suriname" },
    { "SS", "South Sudan" },
    { "ST", "São Tomé and Príncipe" },
    { "SV", "El Salvador" },
    { "SX", "Sint Maarten" },
    { "SY", "Syria" },
    { "SZ", "Eswatini" },
    { "TC", "Turks and Caicos Islands" },
    { "TD", "Chad" },
    { "TF", "French Southern Territories" },
    { "TG", "Togo" },
    { "TH", "Thailand" },
    { "TJ", "Tajikistan" },
    { "TK", "Tokelau" },
    { "TL", "Timor-Leste" },
    { "TM", "Turkmenistan" },
    { "TN", "Tunisia" },
    { "TO", "Tonga" },
    { "TR", "Türkiye" },
    { "TT", "Trinidad and Tobago" },
    { "TV", "Tuvalu" },
    { "TW", "Taiwan" },
    { "TZ", "Tanzania" },
    { "UA", "Ukraine" },
    { "UG", "Uganda" },
    { "UM", "U.S. Minor Outlying Islands" },
    { "US", "United States" },
    { "UY", "Uruguay" },
    { "UZ", "Uzbekistan" },
    { "VA", "Vatican City" },
    { "VC", "Saint Vincent and the Grenadines" },
    { "VE", "Venezuela" },
    { "VG", "British Virgin Islands" },
    { "VI", "U.S. Virgin Islands" },
    { "VN", "Vietnam" },
    { "VU", "Vanuatu" },
    { "WF", "Wallis and Futuna" },
    { "WS", "Samoa" },
    { "XK", "Kosovo" },
    { "YE", "Yemen" },
    { "YT", "Mayotte" },
    { "ZA", "South Africa" },
    { "ZM", "Zambia" },
    { "ZW", "Zimbabwe" },
};

TorCircuitButton::TorCircuitButton(TorManager *manager, QWidget *parent)
    : QToolButton(parent)
    , m_tor(manager)
    , m_menu(new QMenu(this))
{
    setAutoRaise(true);
    setCursor(Qt::ArrowCursor);
    setFocusPolicy(Qt::ClickFocus);
    setPopupMode(QToolButton::InstantPopup);
    setAccessibleName(tr("Tor Circuit"));
    setAccessibleDescription(
        tr("Shows the relays in the current Tor circuit."));
    setIcon(makeIcon());
    setMenu(m_menu);
    connect(m_menu, &QMenu::aboutToShow,
            this, &TorCircuitButton::populateMenu);
    if (m_tor) {
        connect(m_tor, &TorManager::circuitsChanged,
                this, [this](const QList<TorCircuit> &) {
            refreshState();
            if (m_menu->isVisible())
                populateMenu();
        });
        connect(m_tor, &TorManager::stateChanged,
                this, [this](TorManager::State) {
            refreshState();
            if (m_menu->isVisible())
                populateMenu();
        });
    }
    refreshState();
}

QString TorCircuitButton::countryName(const QString &code)
{
    const QString upper = code.trimmed().toUpper();
    if (upper.size() != 2)
        return QString();
    int lo = 0;
    int hi = int(sizeof(kCountryNames) / sizeof(kCountryNames[0])) - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        const int cmp = upper.compare(QLatin1String(kCountryNames[mid][0]));
        if (cmp == 0)
            return QString::fromUtf8(kCountryNames[mid][1]);
        if (cmp < 0)
            hi = mid - 1;
        else
            lo = mid + 1;
    }
    return QString();
}

QIcon TorCircuitButton::flagIcon(const QString &code, qreal devicePixelRatio)
{
    const qreal dpr = devicePixelRatio > 0 ? devicePixelRatio : 1.0;
    const int extent = 16;
    QPixmap pixmap(QSize(extent, extent) * dpr);
    pixmap.setDevicePixelRatio(dpr);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.scale(dpr, dpr);

    const QString upper = code.trimmed().toUpper();
    QString flag;
    // Strictly ASCII A–Z: isLetter() also admits non-ASCII letters,
    // whose distance from 'A' can push the arithmetic past the
    // regional-indicator block (or past Unicode entirely).
    if (upper.size() == 2 && upper.at(0) >= QLatin1Char('A')
            && upper.at(0) <= QLatin1Char('Z')
            && upper.at(1) >= QLatin1Char('A')
            && upper.at(1) <= QLatin1Char('Z')) {
        // Regional indicator symbols: each pair shapes into the
        // country's flag under a color-emoji font; font-less systems
        // draw the two boxed letters, which still carry the code.
        const char32_t pair[2] = {
            char32_t(0x1F1E6) + char32_t(upper.at(0).unicode() - 'A'),
            char32_t(0x1F1E6) + char32_t(upper.at(1).unicode() - 'A'),
        };
        flag = QString::fromUcs4(pair, 2);
    }
    if (!flag.isEmpty()) {
        QFont font = painter.font();
        font.setPixelSize(qRound(extent * 0.8));
        painter.setFont(font);
        painter.drawText(QRect(0, 0, extent, extent), Qt::AlignCenter,
                         flag);
    } else {
        const QColor gray(140, 140, 140);
        painter.setPen(QPen(gray, 1));
        painter.setBrush(Qt::NoBrush);
        painter.drawEllipse(QRectF(1.5, 1.5, extent - 3, extent - 3));
        QFont font = painter.font();
        font.setPixelSize(qRound(extent * 0.7));
        font.setBold(true);
        painter.setFont(font);
        painter.drawText(QRect(0, 0, extent, extent), Qt::AlignCenter,
                         QStringLiteral("?"));
    }
    painter.end();
    return QIcon(pixmap);
}

QString TorCircuitButton::hopText(const TorCircuitHop &hop, int index,
                                  int count)
{
    QString role;
    if (count == 3) {
        role = index == 0 ? tr("Guard")
             : index == count - 1 ? tr("Exit")
             : tr("Middle");
    } else {
        role = tr("Hop %1").arg(index + 1);
    }
    const QString name = hop.nickname.isEmpty()
        ? hop.fingerprint.left(8) : hop.nickname;
    QString country;
    if (!hop.country.isEmpty()) {
        country = countryName(hop.country);
        if (country.isEmpty())
            country = hop.country;
    } else {
        country = tr("Unknown");
    }
    return tr("%1: %2 — %3").arg(role, name, country);
}

bool TorCircuitButton::addCircuitRows(QMenu *menu,
                                      const QList<TorCircuit> &circuits,
                                      int displayId,
                                      qreal devicePixelRatio)
{
    const TorCircuit *circuit = nullptr;
    for (const TorCircuit &candidate : circuits) {
        if (candidate.id == displayId) {
            circuit = &candidate;
            break;
        }
    }
    if (!circuit || circuit->hops.isEmpty())
        return false;
    QString header = tr("Tor circuit #%1 — %2")
        .arg(circuit->id).arg(circuit->status);
    if (!circuit->purpose.isEmpty())
        header += tr(" · %1").arg(circuit->purpose);
    menu->addSection(header);
    for (int i = 0; i < circuit->hops.size(); ++i) {
        const TorCircuitHop &hop = circuit->hops.at(i);
        QAction *row = menu->addAction(
            flagIcon(hop.country, devicePixelRatio),
            hopText(hop, i, circuit->hops.size()));
        // Read-only info — the row does nothing when triggered; the
        // relay's identity details ride the tooltip.
        QStringList details;
        if (!hop.fingerprint.isEmpty())
            details << tr("Fingerprint: %1").arg(hop.fingerprint);
        if (!hop.address.isEmpty())
            details << tr("OR address: %1").arg(hop.address);
        if (!hop.country.isEmpty()) {
            const QString name = countryName(hop.country);
            details << (name.isEmpty()
                ? tr("Country: %1").arg(hop.country)
                : tr("Country: %1 (%2)").arg(name, hop.country));
        }
        row->setToolTip(details.join(QLatin1Char('\n')));
        row->setStatusTip(details.join(QLatin1String(" — ")));
    }
    return true;
}

void TorCircuitButton::populateMenu()
{
    m_menu->clear();
    TorManager *tor = m_tor;
    // Kick a re-query — the reply is collected on the control
    // connection asynchronously, so this never blocks; the fresh
    // snapshot lands through circuitsChanged.
    if (tor)
        tor->requestCircuitInfo();
    if (!tor) {
        m_menu->addSection(tr("Tor control connection unavailable"));
        return;
    }
    switch (tor->state()) {
    case TorManager::Ready:
        // The state can linger at Ready after the daemon drops the
        // control link — serve an honest unavailable state instead of
        // a stale circuit snapshot.
        if (!tor->controlConnected()) {
            m_menu->addSection(
                tr("Tor circuit unavailable — control connection lost"));
            return;
        }
        if (!addCircuitRows(m_menu, tor->circuits(),
                            tor->displayCircuitId(),
                            devicePixelRatioF()))
            m_menu->addSection(tr("No circuit yet"));
        return;
    case TorManager::Failed: {
        m_menu->addSection(tr("Tor: failed"));
        QAction *error = m_menu->addAction(tor->errorString());
        error->setEnabled(false);
        return;
    }
    case TorManager::Stopped:
        m_menu->addSection(tr("Tor daemon is not running"));
        return;
    default: {
        const int progress = tor->bootstrapProgress();
        m_menu->addSection(progress >= 0
            ? tr("Tor: connecting %1%").arg(progress)
            : tr("Tor: connecting..."));
        return;
    }
    }
}

void TorCircuitButton::refreshState()
{
    TorManager *tor = m_tor;
    if (!tor) {
        setEnabled(false);
        setToolTip(tr("Tor control connection unavailable."));
        return;
    }
    setEnabled(true);
    QString tip = tr("Show the current Tor circuit");
    if (tor->state() == TorManager::Ready) {
        const QList<TorCircuit> circuits = tor->circuits();
        for (const TorCircuit &circuit : circuits) {
            if (circuit.id != tor->displayCircuitId())
                continue;
            QStringList names;
            for (const TorCircuitHop &hop : circuit.hops) {
                names << (hop.nickname.isEmpty()
                          ? hop.fingerprint.left(8) : hop.nickname);
            }
            if (!names.isEmpty()) {
                tip = tr("Tor circuit: %1")
                    .arg(names.join(QLatin1String(" -> ")));
            }
            break;
        }
    } else if (tor->state() == TorManager::Failed) {
        tip = tr("Tor failed: %1").arg(tor->errorString());
    }
    setToolTip(tip);
}

void TorCircuitButton::changeEvent(QEvent *event)
{
    // The onion is painted from ButtonText — re-resolve it when the
    // palette changes (the tor chrome lifts it to #eee after
    // construction).
    if (event->type() == QEvent::PaletteChange)
        setIcon(makeIcon());
    QToolButton::changeEvent(event);
}

QIcon TorCircuitButton::makeIcon() const
{
    // A small onion glyph, painted so no icon theme is required — the
    // tor toolbar's ButtonText resolves to #eee on the violet bar.
    QPixmap pixmap(16, 16);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(palette().color(QPalette::ButtonText), 1.15));
    painter.setBrush(Qt::NoBrush);
    QPainterPath bulb;
    bulb.moveTo(8.0, 5.0);                  // tip
    bulb.quadTo(2.2, 7.0, 2.2, 10.8);       // left wall
    bulb.quadTo(2.2, 14.6, 8.0, 14.6);      // bottom-left
    bulb.quadTo(13.8, 14.6, 13.8, 10.8);    // bottom-right
    bulb.quadTo(13.8, 7.0, 8.0, 5.0);       // right wall back to tip
    painter.drawPath(bulb);
    // inner layer curve
    QPainterPath inner;
    inner.moveTo(8.0, 7.0);
    inner.quadTo(4.6, 8.8, 5.0, 12.8);
    painter.drawPath(inner);
    // three leaf sprigs above the tip
    painter.drawLine(QPointF(8.0, 4.6), QPointF(8.0, 1.8));
    painter.drawLine(QPointF(7.5, 4.8), QPointF(5.4, 2.6));
    painter.drawLine(QPointF(8.5, 4.8), QPointF(10.6, 2.6));
    painter.end();
    return QIcon(pixmap);
}
