/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#include "extensioncommands.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "extensionactionbar.h"
#include "extensionbackgroundhost.h"
#include "extensioneventhub.h"
#include "extensionmanager.h"
#include "extensionshimbackend.h"
#include "tabwidget.h"
#include "webview.h"

#include <qapplication.h>
#include <qevent.h>
#include <qjsonarray.h>
#include <qshortcut.h>

ExtensionCommandRegistry *ExtensionCommandRegistry::instance()
{
    static ExtensionCommandRegistry *registry =
        new ExtensionCommandRegistry;
    return registry;
}

ExtensionCommandRegistry::ExtensionCommandRegistry(QObject *parent)
    : QObject(parent)
{
}

// -------------------------------------------------------------------
// Chrome key-string -> QKeySequence.  Chrome writes "Ctrl+Shift+Y"
// with named non-character keys (Comma, PageUp, MediaNextTrack…);
// Qt's sequence parser takes the same '+'-joined shape with its own
// names for a few of them.  Anything that still does not parse
// stays unbound — Chrome shows the command unassigned the same way.
// -------------------------------------------------------------------

QKeySequence ExtensionCommandRegistry::parseKey(const QString &raw)
{
    static const QHash<QString, QString> named = {
        {QStringLiteral("Space"), QStringLiteral("Space")},
        {QStringLiteral("Comma"), QStringLiteral(",")},
        {QStringLiteral("Period"), QStringLiteral(".")},
        {QStringLiteral("Slash"), QStringLiteral("/")},
        {QStringLiteral("Backslash"), QStringLiteral("\\")},
        {QStringLiteral("Semicolon"), QStringLiteral(";")},
        {QStringLiteral("Quote"), QStringLiteral("'")},
        {QStringLiteral("Apostrophe"), QStringLiteral("'")},
        {QStringLiteral("BracketLeft"), QStringLiteral("[")},
        {QStringLiteral("BracketRight"), QStringLiteral("]")},
        {QStringLiteral("Minus"), QStringLiteral("-")},
        {QStringLiteral("Equal"), QStringLiteral("=")},
        {QStringLiteral("Backquote"), QStringLiteral("`")},
        {QStringLiteral("Up"), QStringLiteral("Up")},
        {QStringLiteral("Down"), QStringLiteral("Down")},
        {QStringLiteral("Left"), QStringLiteral("Left")},
        {QStringLiteral("Right"), QStringLiteral("Right")},
        {QStringLiteral("PageUp"), QStringLiteral("PgUp")},
        {QStringLiteral("PageDown"), QStringLiteral("PgDown")},
        {QStringLiteral("Home"), QStringLiteral("Home")},
        {QStringLiteral("End"), QStringLiteral("End")},
        {QStringLiteral("Insert"), QStringLiteral("Ins")},
        {QStringLiteral("Delete"), QStringLiteral("Del")},
        {QStringLiteral("Backspace"), QStringLiteral("Backspace")},
        {QStringLiteral("Tab"), QStringLiteral("Tab")},
        {QStringLiteral("Enter"), QStringLiteral("Return")},
        {QStringLiteral("Return"), QStringLiteral("Return")},
        {QStringLiteral("Escape"), QStringLiteral("Esc")},
    };
    QStringList tokens;
    const QStringList parts = raw.split(QLatin1Char('+'));
    for (const QString &part : parts) {
        const QString token = part.trimmed();
        if (token.isEmpty())
            continue;
        if (token == QLatin1String("Ctrl")
                || token == QLatin1String("Alt")
                || token == QLatin1String("Shift")) {
            tokens.append(token);
        } else if (token == QLatin1String("Command")
                   || token == QLatin1String("MacCtrl")) {
            // macOS spellings — on this platform the closest honest
            // binding is the control key.
            tokens.append(QStringLiteral("Ctrl"));
        } else if (token == QLatin1String("Search")) {
            tokens.append(QStringLiteral("Meta"));
        } else {
            tokens.append(named.value(token, token));
        }
    }
    // fromString is lenient — an unrecognized key name yields a
    // one-chord sequence carrying Key_unknown, which would bind a
    // dead shortcut.  Only a real key is bindable.
    const QKeySequence parsed =
        QKeySequence::fromString(tokens.join(QLatin1Char('+')));
    if (parsed.isEmpty() || parsed[0].key() == Qt::Key_unknown)
        return QKeySequence();
    return parsed;
}

// -------------------------------------------------------------------
// Registry sync — the runtime's desired extension set is the whole
// truth: a departed id loses its bindings (disable/unload/update),
// a new id parses once.  An update reloads under the same id but
// leaves the desired set first, so the next appearance re-reads the
// changed manifest.
// -------------------------------------------------------------------

void ExtensionCommandRegistry::syncExtensions(
        const QHash<QString, QString> &extensionPaths)
{
    const QStringList known = m_extensions.keys();
    for (const QString &id : known) {
        if (!extensionPaths.contains(id))
            dropExtension(id);
    }
    for (auto it = extensionPaths.constBegin();
            it != extensionPaths.constEnd(); ++it) {
        if (!m_extensions.contains(it.key()))
            loadExtension(it.key(), it.value());
    }
    armAllWindows();
    setFilterInstalled(!m_extensions.isEmpty());
}

void ExtensionCommandRegistry::dropExtension(const QString &extensionId)
{
    m_extensions.remove(extensionId);
    const QString prefix = extensionId + QLatin1Char('\x1f');
    for (auto it = m_bound.begin(); it != m_bound.end();) {
        if (it.key().startsWith(prefix)) {
            for (const QPointer<QShortcut> &sc : it.value())
                delete sc.data();
            it = m_bound.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = m_noted.begin(); it != m_noted.end();) {
        if (it->startsWith(prefix))
            it = m_noted.erase(it);
        else
            ++it;
    }
    m_pendingNotes.remove(extensionId);
    if (m_extensions.isEmpty())
        setFilterInstalled(false);
}

void ExtensionCommandRegistry::loadExtension(
        const QString &extensionId, const QString &path)
{
    Entry entry;
    entry.path = path;
    // The manifest check is the sanctioned parse path (Rust verdict
    // under rustcore) — never a raw JSON read here.
    const ExtensionManager::Manifest manifest =
        ExtensionManager::inspectManifest(path);
    if (manifest.valid) {
        entry.hasAction = manifest.hasAction;
        entry.actionPopup = manifest.actionPopup;
        const QJsonObject commands = manifest.commands;
        for (auto it = commands.constBegin();
                it != commands.constEnd(); ++it) {
            const QString name = it.key();
            const QJsonObject spec = it.value().toObject();
            // Reserved MV2 names — Chrome never serves them under
            // manifest_version 3 either; declaring one earns a note,
            // not a binding.
            if (name == QLatin1String("_execute_browser_action")
                    || name == QLatin1String("_execute_page_action")) {
                noteTo(extensionId, QStringLiteral(
                    "commands.%1: reserved MV2 command name — "
                    "not bound").arg(name));
                continue;
            }
            Command cmd;
            cmd.name = name;
            cmd.description = spec.value(QStringLiteral("description"))
                .toString();
            cmd.global = spec.value(QStringLiteral("global")).toBool();
            const QJsonObject keys =
                spec.value(QStringLiteral("suggested_key")).toObject();
            cmd.suggestedKey =
                keys.value(QStringLiteral("linux")).toString();
            if (cmd.suggestedKey.isEmpty())
                cmd.suggestedKey =
                    keys.value(QStringLiteral("default")).toString();
            if (!cmd.suggestedKey.isEmpty())
                cmd.key = parseKey(cmd.suggestedKey);
            if (!cmd.suggestedKey.isEmpty() && cmd.key.isEmpty())
                noteTo(extensionId, QStringLiteral(
                    "commands.%1: suggested_key '%2' is not bindable")
                        .arg(name, cmd.suggestedKey));
            if (cmd.global)
                noteTo(extensionId, QStringLiteral(
                    "commands.%1: 'global' unsupported — bound "
                    "window-scoped only").arg(name));
            entry.commands.append(cmd);
        }
    }
    m_extensions.insert(extensionId, entry);
}

// -------------------------------------------------------------------
// Arming — one WindowShortcut per command per window.  The filter
// catches windows shown after registration; armAllWindows covers
// the ones already up when a binding lands.
// -------------------------------------------------------------------

bool ExtensionCommandRegistry::eventFilter(QObject *watched,
                                           QEvent *event)
{
    if ((event->type() == QEvent::Show
            || event->type() == QEvent::WindowActivate)
            && qobject_cast<BrowserMainWindow *>(watched))
        armWindow(static_cast<BrowserMainWindow *>(watched));
    return QObject::eventFilter(watched, event);
}

void ExtensionCommandRegistry::setFilterInstalled(bool on)
{
    if (m_filterInstalled == on || !qApp)
        return;
    m_filterInstalled = on;
    if (on)
        qApp->installEventFilter(this);
    else
        qApp->removeEventFilter(this);
}

void ExtensionCommandRegistry::armAllWindows()
{
    // Private/tor windows never get extension shortcuts — the flag
    // is app-global and covers both (every window in such a session
    // browses on a profile extensions cannot touch).
    if (BrowserApplication::isPrivate())
        return;
    const QList<BrowserMainWindow *> windows =
        ExtensionEventHub::browsingWindows();
    for (BrowserMainWindow *window : windows)
        armWindow(window);
}

void ExtensionCommandRegistry::armWindow(BrowserMainWindow *window)
{
    if (!window || BrowserApplication::isPrivate())
        return;
    for (auto it = m_extensions.constBegin();
            it != m_extensions.constEnd(); ++it) {
        const QString extId = it.key();
        for (const Command &cmd : it.value().commands) {
            if (cmd.key.isEmpty())
                continue;
            const QString boundKey =
                extId + QLatin1Char('\x1f') + cmd.name;
            QList<QPointer<QShortcut>> &shortcuts = m_bound[boundKey];
            bool bound = false;
            for (const QPointer<QShortcut> &sc : shortcuts) {
                if (sc && sc->parent() == window) {
                    bound = true;
                    break;
                }
            }
            if (bound)
                continue;
            const QString name = cmd.name;
            auto *sc = new QShortcut(cmd.key, window);
            sc->setContext(Qt::WindowShortcut);
            connect(sc, &QShortcut::activated, this,
                    [this, extId, name, window]() {
                trigger(extId, name, window);
            });
            // An ambiguous key (a second claimant — another
            // extension's command or a browser binding) fires
            // neither side; the denial is recorded, not silent.
            connect(sc, &QShortcut::activatedAmbiguously, this,
                    [this, extId, name]() {
                noteTo(extId, QStringLiteral(
                    "commands.%1: shortcut is ambiguous — "
                    "not dispatched").arg(name));
            });
            shortcuts.append(sc);
        }
    }
}

// -------------------------------------------------------------------
// Dispatch — Chrome's signatures: commands.onCommand(command, tab),
// action.onClicked(tab); _execute_action never reaches onCommand,
// it performs the action click itself.
// -------------------------------------------------------------------

void ExtensionCommandRegistry::trigger(
        const QString &extensionId, const QString &name,
        BrowserMainWindow *window)
{
    const Entry entry = m_extensions.value(extensionId);

    // The tab argument Chrome passes — the active tab of the window
    // the key fired in.  An off-the-record tab is invisible to
    // extensions, so it yields a null tab rather than leaking its
    // metadata into the payload.
    QJsonValue tab(QJsonValue::Null);
    WebView *view = nullptr;
    if (window && window->tabWidget())
        view = window->tabWidget()->currentWebView();
    if (ExtensionEventHub::trackable(view)) {
        tab = ExtensionShimBackend::tabObject(
            view, window->tabWidget()->webViewIndex(view),
            ExtensionShimBackend::windowIdFor(window));
    }

    ExtensionBackgroundRuntime *runtime =
        ExtensionBackgroundRuntime::instance();
    if (name == QLatin1String("_execute_action")) {
        if (!entry.hasAction) {
            noteTo(extensionId, QStringLiteral(
                "commands._execute_action: no action declared — "
                "not dispatched"));
            return;
        }
        const ExtensionShimBackend::ActionState state =
            ExtensionShimBackend::actionState(extensionId);
        // A disabled action does not fire at all — Chrome ignores
        // both its click and its command binding.
        if (!state.enabled) {
            noteTo(extensionId, QStringLiteral(
                "commands._execute_action: action is disabled — "
                "not dispatched"));
            return;
        }
        // chrome.action.setPopup can override the declared popup at
        // runtime ("" removes it entirely -> onClicked fires).
        const QString popup = state.popupSet
            ? state.popup : entry.actionPopup;
        if (!popup.isEmpty()) {
            ExtensionActionBar *bar =
                window ? window->findChild<ExtensionActionBar *>()
                       : nullptr;
            if (bar && bar->triggerAction(extensionId, popup))
                return;
            noteTo(extensionId, QStringLiteral(
                "commands._execute_action: action popup could not "
                "be opened"));
            return;
        }
        runtime->deliverEvent(extensionId,
            QStringLiteral("action.onClicked"),
            QJsonObject{{QStringLiteral("__args"),
                         QJsonArray{tab}}});
        return;
    }
    runtime->deliverEvent(extensionId,
        QStringLiteral("commands.onCommand"),
        QJsonObject{{QStringLiteral("__args"),
                     QJsonArray{name, tab}}});
}

// -------------------------------------------------------------------
// Diagnostics
// -------------------------------------------------------------------

void ExtensionCommandRegistry::noteTo(const QString &extensionId,
                                      const QString &text)
{
    const QString key = extensionId + QLatin1Char('\x1f') + text;
    if (m_noted.contains(key))
        return;
    m_noted.insert(key);
    m_pendingNotes[extensionId].append(text);
    flushNotes(extensionId);
}

void ExtensionCommandRegistry::flushNotes(const QString &extensionId)
{
    const QStringList pending = m_pendingNotes.value(extensionId);
    if (pending.isEmpty())
        return;
    ExtensionBackgroundHost *host =
        ExtensionBackgroundRuntime::instance()->host(extensionId);
    ExtensionShimBackend *backend =
        host && host->bridge() ? host->bridge()->backend() : nullptr;
    if (!backend)
        return;
    for (const QString &text : pending)
        backend->recordNote(text);
    m_pendingNotes.remove(extensionId);
}

QStringList ExtensionCommandRegistry::boundCommands(
        const QString &extensionId) const
{
    QStringList out;
    for (const Command &cmd : m_extensions.value(extensionId).commands)
        if (!cmd.key.isEmpty())
            out.append(cmd.name);
    return out;
}

QStringList ExtensionCommandRegistry::unboundCommands(
        const QString &extensionId) const
{
    QStringList out;
    for (const Command &cmd : m_extensions.value(extensionId).commands)
        if (cmd.key.isEmpty())
            out.append(cmd.name);
    return out;
}

QJsonArray ExtensionCommandRegistry::commandList(
        const QString &extensionId) const
{
    QJsonArray out;
    for (const Command &cmd : m_extensions.value(extensionId).commands) {
        QJsonObject row;
        row.insert(QStringLiteral("name"), cmd.name);
        row.insert(QStringLiteral("description"), cmd.description);
        // Bound -> the Chrome-format key that parsed; unbound -> ''
        // like Chrome's unassigned rows (the declared suggestion is
        // not reported as assigned).
        row.insert(QStringLiteral("shortcut"),
                   cmd.key.isEmpty() ? QString() : cmd.suggestedKey);
        out.append(row);
    }
    return out;
}

int ExtensionCommandRegistry::shortcutCount() const
{
    int n = 0;
    for (const QList<QPointer<QShortcut>> &shortcuts : m_bound)
        for (const QPointer<QShortcut> &sc : shortcuts)
            if (sc)
                ++n;
    return n;
}
