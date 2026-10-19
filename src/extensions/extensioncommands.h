/*
 * Copyright 2026 The Arktos Authors
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

#ifndef EXTENSIONCOMMANDS_H
#define EXTENSIONCOMMANDS_H

#include <qhash.h>
#include <qjsonarray.h>
#include <qkeysequence.h>
#include <qlist.h>
#include <qobject.h>
#include <qpointer.h>
#include <qset.h>
#include <qstring.h>
#include <qstringlist.h>

class BrowserMainWindow;
class QShortcut;

// ER14 — chrome.commands, served by the hosted runtime.  Qt's
// extension embedder never registered the commands delegate, so
// manifest-declared keyboard shortcuts are bound and dispatched
// here.
//
// What this is: the manifest "commands" map — parsed by
// ExtensionManager::inspectManifest (rc_ext_manifest_check under
// rustcore, its Qt fallback otherwise) — becomes one QShortcut per
// browsing window for every command with a bindable suggested_key
// (the platform key, then "default").  A fired shortcut dispatches
// commands.onCommand(command, tab) into the owning extension's
// shim contexts through the runtime's deliverEvent; the reserved
// _execute_action instead performs the action click itself —
// opening the declared popup through the window's
// ExtensionActionBar, or firing action.onClicked when the action
// carries no popup.
//
// Binding model: one Qt::WindowShortcut per command per top-level
// BrowserMainWindow, so the command acts on the focused window's
// active tab like Chrome's does.  Windows shown later are armed by
// an app-level event filter that exists only while any command is
// registered — an extension-free session installs nothing.
// Private/tor windows are never armed (BrowserApplication::
// isPrivate() covers both) and the tab argument is dropped when the
// window's active tab is an off-the-record view.
//
// Honest limits, each recorded once into the extension's health
// notes rather than swallowed:
//   - an unparsable or unbindable suggested_key leaves the command
//     unbound — Chrome shows it unassigned the same way;
//   - "global": true still binds, window-scoped — there is no
//     OS-global shortcut channel on this stack;
//   - an ambiguous key (two extension commands claiming it, or a
//     browser binding winning the shortcut map) dispatches nothing
//     and records the ambiguity;
//   - the reserved MV2 names _execute_browser_action /
//     _execute_page_action are never bound, as Chrome does under
//     MV3;
//   - commands without a suggested_key stay unbound but remain in
//     chrome.commands.getAll with an empty shortcut, matching
//     Chrome — there is no shortcut-remap UI yet, which is the
//     documented gap.
//
// chrome.commands.getAll rides the bridge into
// ExtensionShimBackend::invokeCommands and is answered from
// commandList — the bound-state rows above, not the manifest text,
// so an unbindable suggestion reports '' like Chrome's unassigned
// rows do.
class ExtensionCommandRegistry : public QObject
{
    Q_OBJECT

public:
    static ExtensionCommandRegistry *instance();

    // Driven from ExtensionBackgroundRuntime::syncNow with the
    // desired set — every enabled+loaded extension id -> its
    // unpacked path.  New ids parse and arm, departed ids drop
    // their bindings; a respawned id re-parses so an update's
    // changed manifest rebinds.
    void syncExtensions(
        const QHash<QString, QString> &extensionPaths);
    void dropExtension(const QString &extensionId);
    // Delivers fidelity notes recorded before the extension's host
    // existed — parse-time findings sit in the pending queue until
    // the host's Ready transition calls this.
    void flushNotes(const QString &extensionId);

    // Performs one command dispatch — the QShortcut::activated
    // handlers land here; tests may drive the same path directly.
    void trigger(const QString &extensionId, const QString &name,
                 BrowserMainWindow *window);

    // Diagnostics/test seams.
    QStringList boundCommands(const QString &extensionId) const;
    QStringList unboundCommands(const QString &extensionId) const;
    // Chrome-shaped chrome.commands.getAll rows — every declared
    // command (except the never-bound MV2 reserved names) with the
    // CURRENTLY bound shortcut in Chrome key format, '' when the
    // suggestion was absent or unbindable, exactly like Chrome's
    // unassigned rows.
    QJsonArray commandList(const QString &extensionId) const;
    int shortcutCount() const;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    explicit ExtensionCommandRegistry(QObject *parent = nullptr);

    struct Command {
        QString name;
        QString description;
        QString suggestedKey;   // raw chrome key string (platform
                                // pick already applied)
        QKeySequence key;       // empty => unbound
        bool global = false;
    };
    struct Entry {
        QString path;
        QList<Command> commands;
        bool hasAction = false;
        QString actionPopup;    // action.default_popup; '' = none
    };

    void loadExtension(const QString &extensionId,
                       const QString &path);
    // Arms every bindable command of every known extension on one
    // window — deduped by (command, window), so repeat calls are
    // cheap.
    void armWindow(BrowserMainWindow *window);
    void armAllWindows();
    void noteTo(const QString &extensionId, const QString &text);
    void setFilterInstalled(bool on);

    static QKeySequence parseKey(const QString &raw);

    QHash<QString, Entry> m_extensions;
    // "<extId>\x1f<command>" -> the live per-window shortcuts; the
    // QPointers go stale-free when a window dies with its children.
    QHash<QString, QList<QPointer<QShortcut>>> m_bound;
    // extId + '\x1f' + text — a fidelity note is emitted once per
    // extension lifetime, not once per heartbeat.
    QSet<QString> m_noted;
    // extId -> notes recorded while no host existed to hold them;
    // flushNotes drains it on the host's Ready transition.
    QHash<QString, QStringList> m_pendingNotes;
    bool m_filterInstalled = false;
};

#endif // EXTENSIONCOMMANDS_H
