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

#ifndef WEBPERMISSIONMANAGER_H
#define WEBPERMISSIONMANAGER_H

#include <qhash.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qurl.h>
#include <qwebenginepermission.h>

class QWidget;

// SEC05: default-deny broker for QWebEnginePage::permissionRequested.
//
// Qt WebEngine emits a QWebEnginePermission for every feature request
// (notifications, geolocation, capture devices, clipboard, ...).  Left
// unanswered the request hangs and the site silently gets nothing; a
// blanket grant would be worse.  This manager resolves every request:
//
//   - Capture devices, desktop/screen capture, clipboard read and
//     local-font enumeration are denied outright — they are never
//     prompted for, so a user can never be one mis-click away from a
//     live microphone.
//   - Notifications, geolocation and mouse lock raise a consent
//     prompt identifying the requesting origin and the feature.
//   - "Remember this decision" stores the answer per site and feature
//     in QSettings (group "webpermissions"); the Settings dialog lists
//     the entries and can revoke them.  Remembered denies are honored
//     the same way as remembered grants.
//   - Every answered prompt is also kept for the session so a page
//     cannot loop a request into a modal storm, and off-the-record
//     pages never write to the persistent store at all.
//
// The profile's PersistentPermissionsPolicy is left at AskEveryTime:
// remembering is owned here, not by Chromium, which keeps the store
// auditable and independent of the profile's on-disk internals.
class WebPermissionManager : public QObject
{
    Q_OBJECT

public:
    struct Entry {
        QUrl origin;
        QWebEnginePermission::PermissionType type;
        bool granted;
    };

    explicit WebPermissionManager(QObject *parent = nullptr);

    // Lazy qApp-owned singleton (HistoryManager pattern); the class is
    // still directly constructible for autotests.
    static WebPermissionManager *instance();

    // Feature policy.
    static bool isDenied(QWebEnginePermission::PermissionType type);
    static bool isPromptable(QWebEnginePermission::PermissionType type);
    // Translated display name ("Notifications", "Geolocation", ...).
    static QString typeName(QWebEnginePermission::PermissionType type);
    // Translated verb phrase for the consent prompt ("show
    // notifications", "know your location", ...).
    static QString typeDescription(QWebEnginePermission::PermissionType type);

    // The remembered decisions, for the auditable Settings list.
    QList<Entry> entries() const;
    void setEntry(const QUrl &origin, QWebEnginePermission::PermissionType type,
                  bool granted);
    void removeEntry(const QUrl &origin, QWebEnginePermission::PermissionType type);
    void clearEntries();

    // Called from WebPage's permissionRequested handler.  The request
    // is resolved synchronously or (for prompts) asynchronously —
    // QWebEnginePermission stays resolvable until grant()/deny().
    void handleRequest(QWidget *parent, const QWebEnginePermission &request,
                       bool offTheRecord);

signals:
    void changed();
    // Emitted right before a consent dialog opens — lets tests (and
    // future UI) observe prompts without peeking at modal widgets.
    void promptRequested(const QUrl &origin,
                         QWebEnginePermission::PermissionType type);

private slots:
    void showNextPrompt();

private:
    static QString keyFor(const QUrl &origin,
                          QWebEnginePermission::PermissionType type);
    // True when a remembered decision exists; *granted carries it.
    // Off-the-record pages share the persistent store read-only but
    // get a separate session namespace so an OTR answer can never
    // bleed into normal browsing (or vice versa).
    bool storedDecision(const QUrl &origin,
                        QWebEnginePermission::PermissionType type,
                        bool *granted, bool offTheRecord) const;
    static QString sessionKeyFor(const QUrl &origin,
                        QWebEnginePermission::PermissionType type,
                        bool offTheRecord);

    struct Pending {
        QPointer<QWidget> parent;
        QWebEnginePermission permission;
        bool offTheRecord;
    };
    QList<Pending> m_pending;
    // Guards showNextPrompt against re-entry through the modal exec()
    // loop — a second request must queue, not stack a dialog.
    bool m_inPrompt;
    // "encoded-origin|type" -> granted; answers that were not persisted
    // (remember unchecked, or an off-the-record page) live here for the
    // session so a page cannot spam prompts.
    QHash<QString, bool> m_sessionDecisions;
};

#endif // WEBPERMISSIONMANAGER_H
