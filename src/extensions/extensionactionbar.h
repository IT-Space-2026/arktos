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

#ifndef EXTENSIONACTIONBAR_H
#define EXTENSIONACTIONBAR_H

#include <qhash.h>
#include <qpointer.h>
#include <qsize.h>
#include <qstring.h>
#include <qurl.h>
#include <qwidget.h>

class QFrame;
class QHBoxLayout;
class QToolButton;

// EXT08: per-window extension action strip — Chrome's
// between-omnibox-and-menu cluster.  One flat QToolButton per
// enabled+loaded extension whose manifest declares an action popup;
// the click opens a Qt::Popup-framed QWebEngineView on the canonical
// browsing profile loading the extension's actionPopupUrl.  The
// whole strip hides while zero popup-bearing extensions are enabled
// (no dead gap in the splitter), and private/tor windows never
// construct it at all — no profile they run may load extensions
// (EXT04/EXT05).
//
// The set tracks ExtensionManager's lifecycle signals: load/unload,
// install/uninstall and the changed() pulse that follows every
// enable/disable toggle.  Buttons are rebuilt wholesale on each
// signal — transitions are user-driven and rare, so the simple full
// rebuild keeps every state transition correct by construction.
class ExtensionActionBar : public QWidget
{
    Q_OBJECT

public:
    explicit ExtensionActionBar(QWidget *parent = nullptr);

    // Matches the hosting toolbar's icon size — applied to every
    // action button as they are (re)built.
    void setIconSize(const QSize &size);
    QSize iconSize() const;

    // Number of action buttons currently hosted — exposed for tests.
    int actionCount() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
    void rebuild();

private:
    void openPopup(const QString &id, const QUrl &popupUrl,
                   QToolButton *anchor);

    QHBoxLayout *m_layout;
    QSize m_iconSize;
    // The one open action popup and the extension it serves — a
    // second click on the same button toggles it closed.
    QPointer<QFrame> m_popup;
    QString m_popupExtensionId;
};

#endif // EXTENSIONACTIONBAR_H
