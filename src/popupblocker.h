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

#ifndef POPUPBLOCKER_H
#define POPUPBLOCKER_H

#include <qobject.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

class QUrl;

// POPUP01: real pop-up blocking.
//
// QWebEngineSettings::JavascriptCanOpenWindows=false only withholds
// window.open calls that lack a user gesture — click-triggered
// featureful pop-ups still reach WebPage::createWindow.  This class
// is the app-side gate for those: createWindow asks shouldBlock()
// with the OPENER page's url and dead-ends the request when the
// blocker is on and the opener's host is not excepted.
//
// The allow list keys on the site SHOWING the pop-up, matching
// Firefox's "Block pop-up windows -> Exceptions" semantics: a rule
// recorded on "example.com" covers "www.example.com".  Two layers
// exist like ScriptControlManager's rules:
//
//   - persistent hosts, written to QSettings (group
//     "popupExceptions", key "allowed") and audited through the
//     Settings dialog's Site Permissions list;
//   - session hosts, held in memory only — off-the-record pages
//     never persist.
class PopupBlocker : public QObject
{
    Q_OBJECT

public:
    explicit PopupBlocker(QObject *parent = nullptr);

    // Lazy qApp-owned singleton (HistoryManager pattern); the class is
    // still directly constructible for autotests.
    static PopupBlocker *instance();

    // The Settings > Privacy "Block Popup Windows" checkbox owns this
    // flag (websettings/blockPopupWindows) so existing profiles carry
    // over — read live so a settings change applies without waiting
    // for the next profile settings pass.
    bool isEnabled() const;
    void setEnabled(bool enabled);

    // The createWindow gate: blocking applies to http/https pages
    // with a host — internal pages and host-less urls (file:, data:,
    // about:, qrc:, arora-*) keep their windows since the allow list
    // could not express them anyway.
    bool shouldBlock(const QUrl &openerUrl) const;

    // Whether the opener page's host is excepted (session or
    // persistent layer).
    bool isAllowed(const QUrl &openerUrl) const;
    bool isAllowedHost(const QString &host) const;

    // Records an exception.  persistent=false keeps it in the session
    // overlay — that is what off-the-record pages use.
    void allowHost(const QString &host, bool persistent = true);
    void removeAllowedHost(const QString &host);

    // The persistent list, for the Settings audit table.
    QStringList allowedHosts() const { return m_allowed; }
    void clearAllowedHosts();

    // Test hook: drop the session overlay (persistent hosts stay).
    void clearSessionHosts();

signals:
    // Emitted when any exception changes so open panels/lists can
    // refresh.
    void changed();

private:
    static QString normalizeHost(const QString &host);
    static bool matchesHost(const QStringList &list, const QString &host);
    static bool matchesHost(const QSet<QString> &set, const QString &host);
    void save() const;

    QStringList m_allowed;
    QSet<QString> m_sessionAllowed;
};

#endif // POPUPBLOCKER_H
