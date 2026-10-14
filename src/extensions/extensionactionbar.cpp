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

#include "extensionactionbar.h"

#include "aroraicon.h"
#include "browserapplication.h"
#include "extensionmanager.h"

#include <qevent.h>
#include <qfile.h>
#include <qframe.h>
#include <qguiapplication.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qlayout.h>
#include <qscreen.h>
#include <qtoolbutton.h>
#include <qvariant.h>

#include <qwebenginepage.h>
#include <qwebengineprofile.h>
#include <qwebengineview.h>

namespace {

// Manifest-V3 icon picking: "action".default_icon (a single path or a
// size->path map) wins; the top-level "icons" map is the fallback.
// Best fit = the smallest declared size >= target, else the largest
// available.  Returns a path relative to the extension dir, empty
// when the manifest declares nothing usable.
QString manifestIconPath(const QString &extensionPath, int targetSize)
{
    if (extensionPath.isEmpty())
        return QString();
    QFile file(extensionPath + QLatin1String("/manifest.json"));
    if (file.size() > 1024 * 1024 || !file.open(QIODevice::ReadOnly))
        return QString();
    const QJsonObject root =
        QJsonDocument::fromJson(file.readAll()).object();

    const auto collect = [](const QJsonValue &value,
                            QMap<int, QString> *candidates) {
        if (value.isString()) {
            // Size unknown — treat as mid-size so explicit sizes win.
            candidates->insert(16, value.toString());
        } else if (value.isObject()) {
            const QJsonObject sizes = value.toObject();
            for (auto it = sizes.begin(); it != sizes.end(); ++it)
                candidates->insert(it.key().toInt(), it.value().toString());
        }
    };

    QMap<int, QString> candidates;
    for (const char *key : {"action", "browser_action", "page_action"})
        collect(root.value(QLatin1String(key))
                    .toObject().value(QLatin1String("default_icon")),
                &candidates);
    if (candidates.isEmpty())
        collect(root.value(QLatin1String("icons")), &candidates);

    // Smallest declared size >= target, else the largest available.
    int best = -1;
    int largest = -1;
    for (auto it = candidates.constBegin(); it != candidates.constEnd(); ++it) {
        if (it.value().isEmpty())
            continue;
        if (it.key() > largest)
            largest = it.key();
        if (it.key() >= targetSize && (best < 0 || it.key() < best))
            best = it.key();
    }
    if (best < 0)
        best = largest;
    return best >= 0 ? candidates.value(best) : QString();
}

// Extension glyph at toolbar size — the manifest's own artwork when
// it ships any, else the generic add-on glyph through the icon theme
// (native sets carry a real application-x-addon; the bundled art is
// the fallback).  Cached by extension id — icon files change only
// with an extension update, which reloads the extension anyway.
QIcon extensionIcon(const ExtensionManager::ExtensionInfo &info, int edge)
{
    static QHash<QString, QIcon> cache;
    const QString key = info.id + QLatin1Char('@') + QString::number(edge);
    const auto cached = cache.constFind(key);
    if (cached != cache.constEnd())
        return cached.value();

    QIcon icon;
    const QString rel = manifestIconPath(info.path, edge);
    if (!rel.isEmpty()) {
        const QPixmap pixmap(info.path + QLatin1Char('/') + rel);
        if (!pixmap.isNull())
            icon = QIcon(pixmap.scaled(edge, edge,
                                       Qt::KeepAspectRatio,
                                       Qt::SmoothTransformation));
    }
    if (icon.isNull())
        // A chrome stand-in, not identity art — tinted house style
        // so it does not render a raw theme glyph beside chrome peers.
        icon = AroraIcon::getChrome(QLatin1String("application-x-addon"));

    // The cache is a per-session convenience, not a registry — bound
    // it so a pathological load/unload loop cannot grow it forever.
    if (cache.size() > 128)
        cache.clear();
    cache.insert(key, icon);
    return icon;
}

} // namespace

ExtensionActionBar::ExtensionActionBar(QWidget *parent)
    : QWidget(parent)
    , m_layout(new QHBoxLayout(this))
{
    setObjectName(QLatin1String("extensionActionBar"));
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->setSpacing(2);
    m_iconSize = QSize(18, 18);

    ExtensionManager *extensions = ExtensionManager::instance();
    // changed() rides along with every finished signal and is also
    // the (delayed) enable/disable pulse — connect all five anyway so
    // the strip reflects load/unload the moment Chromium settles.
    connect(extensions, &ExtensionManager::extensionLoaded,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionUnloaded,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionInstalled,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::extensionUninstalled,
            this, &ExtensionActionBar::rebuild);
    connect(extensions, &ExtensionManager::changed,
            this, &ExtensionActionBar::rebuild);

    rebuild();
}

void ExtensionActionBar::setIconSize(const QSize &size)
{
    if (m_iconSize == size)
        return;
    m_iconSize = size;
    rebuild();
}

QSize ExtensionActionBar::iconSize() const
{
    return m_iconSize;
}

int ExtensionActionBar::actionCount() const
{
    return m_layout->count();
}

void ExtensionActionBar::rebuild()
{
    QList<ExtensionManager::ExtensionInfo> actions;
    if (ExtensionManager::isSupported()) {
        const QList<ExtensionManager::ExtensionInfo> all =
            ExtensionManager::instance()->extensions();
        for (const ExtensionManager::ExtensionInfo &info : all) {
            if (info.loaded && info.enabled
                && !info.actionPopupUrl.isEmpty())
                actions.append(info);
        }
    }

    while (QLayoutItem *item = m_layout->takeAt(0))
        delete item->widget();

    const int edge = qMax(m_iconSize.width(), m_iconSize.height());
    for (const ExtensionManager::ExtensionInfo &info : actions) {
        QToolButton *button = new QToolButton(this);
        button->setObjectName(
            QLatin1String("extensionActionButton_") + info.id);
        button->setProperty("extensionId", info.id);
        button->setAutoRaise(true);
        button->setCursor(Qt::ArrowCursor);
        button->setFocusPolicy(Qt::ClickFocus);
        button->setIconSize(m_iconSize);
        button->setIcon(extensionIcon(info, edge));
        button->setToolTip(info.name);
        button->setAccessibleName(info.name);
        const QString id = info.id;
        const QUrl popupUrl = info.actionPopupUrl;
        connect(button, &QToolButton::clicked, this,
                [this, id, popupUrl, button]() {
            openPopup(id, popupUrl, button);
        });
        m_layout->addWidget(button);
    }

    // No popup-bearing extensions => no strip and no splitter gap.
    setVisible(!actions.isEmpty());
}

bool ExtensionActionBar::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == m_popup && event->type() == QEvent::KeyPress) {
        QKeyEvent *key = static_cast<QKeyEvent *>(event);
        if (key->key() == Qt::Key_Escape) {
            m_popup->close();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

void ExtensionActionBar::openPopup(const QString &id, const QUrl &popupUrl,
                                   QToolButton *anchor)
{
    // Chrome toggles: re-clicking the serving button closes the popup.
    if (m_popup && m_popupExtensionId == id) {
        m_popup->close();
        return;
    }
    delete m_popup;

    QWebEngineProfile *profile =
        BrowserApplication::instance()->webEngineProfile();
    if (!profile || popupUrl.isEmpty())
        return;

    // Owned by the bar (an open popup dies with its window) and
    // deleted rather than hidden on close — a live WebEngine page is
    // too heavy to keep around after the bubble dismisses, and an
    // orphaned page outlives the profile at teardown.
    QFrame *popup = new QFrame(this, Qt::Popup);
    popup->setAttribute(Qt::WA_DeleteOnClose);
    popup->setObjectName(QLatin1String("extensionActionPopup"));
    popup->setFrameShape(QFrame::StyledPanel);
    QVBoxLayout *layout = new QVBoxLayout(popup);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    QWebEngineView *view = new QWebEngineView(popup);
    // A plain page on the extension's own profile — deliberately not
    // the app's WebPage subclass, so no arora QWebChannel objects or
    // app page policy leak into extension chrome (SEC24 split).
    QWebEnginePage *page = new QWebEnginePage(profile, view);
    view->setPage(page);
    layout->addWidget(view);
    popup->resize(360, 500);

    // Chrome resizes the bubble to the document — WebEngine cannot
    // report a preferred size up front, so after each load the doc's
    // scroll size re-fits the popup within the usual bubble bounds.
    QPointer<QWebEnginePage> pageGuard(page);
    connect(view, &QWebEngineView::loadFinished, popup,
            [popup, pageGuard, anchor]() {
        if (!pageGuard)
            return;
        pageGuard->runJavaScript(
            QStringLiteral("[document.documentElement.scrollWidth,"
                           "document.documentElement.scrollHeight]"),
            [popup, anchor](const QVariant &result) {
                if (!popup || !anchor)
                    return;
                const QVariantList size = result.toList();
                if (size.size() != 2)
                    return;
                popup->resize(
                    qBound(280, size.at(0).toInt() + 24, 800),
                    qBound(120, size.at(1).toInt() + 24, 600));
                const QPoint topRight = anchor->mapToGlobal(
                    QPoint(anchor->width() + 1, anchor->height() + 1));
                popup->move(topRight.x() - popup->width(),
                            topRight.y() + 2);
            });
    });

    popup->installEventFilter(this);
    view->load(popupUrl);

    const QPoint topRight = anchor->mapToGlobal(
        QPoint(anchor->width() + 1, anchor->height() + 1));
    popup->move(topRight.x() - popup->width(), topRight.y() + 2);
    QScreen *screen = QGuiApplication::screenAt(anchor->mapToGlobal(
        anchor->rect().center()));
    if (screen) {
        const QRect available = screen->availableGeometry();
        popup->move(
            qBound(available.left(), popup->x(),
                   qMax(available.left(),
                        available.right() - popup->width() + 1)),
            qBound(available.top(), popup->y(),
                   qMax(available.top(),
                        available.bottom() - popup->height() + 1)));
    }
    popup->show();
    view->setFocus();

    m_popup = popup;
    m_popupExtensionId = id;
}
