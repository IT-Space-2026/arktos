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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "webkitbackend.h"

// glib/WPE headers declare a struct member named `signals` — Qt's
// keyword macros must be out of the way for the include (the harness
// used no_keywords; here the undef is scoped to this TU so the rest
// of the app keeps the signal/slot spellings — use the Q_EMIT forms
// below the include if this file ever needs them).
#ifdef ARKTOS_WEBKIT_LINKED
#ifdef signals
#undef signals
#endif
#ifdef slots
#undef slots
#endif
#ifdef emit
#undef emit
#endif
#include <wpe/webkit.h>
#endif

#include "engineregistry.h"

#include <qapplication.h>

// ---- WebkitView -----------------------------------------------------

WebkitView::WebkitView(QWidget *parent)
    : QWidget(parent)
{
}

// ---- WebkitPage -----------------------------------------------------
//
// Every member is an honest no-op: no WebKitWebView/WPEView exists
// until WK08, so there is nothing to load into, signal from, or tear
// down.  The class is unreachable while the backend refuses pages.

WebkitPage::WebkitPage(Engine::Profile *profile, QObject *parent)
    : Engine::Page(parent)
    , m_profile(profile)
{
}

void WebkitPage::load(const QUrl &url)
{
    // WK08: webkit_web_view_load_uri.  No committed navigation means
    // url() stays empty — recording the request as current would fake
    // a load that never ran.
    Q_UNUSED(url);
}

void WebkitPage::stop()
{
}

void WebkitPage::reload()
{
}

QUrl WebkitPage::url() const
{
    return m_url;
}

bool WebkitPage::canGoBack() const
{
    return false;
}

bool WebkitPage::canGoForward() const
{
    return false;
}

void WebkitPage::back()
{
}

void WebkitPage::forward()
{
}

int WebkitPage::historyCount() const
{
    return 0;
}

int WebkitPage::currentHistoryIndex() const
{
    return -1;
}

QList<Engine::HistoryEntry> WebkitPage::historyItems() const
{
    return {};
}

QList<Engine::HistoryEntry> WebkitPage::backItems(int maxItems) const
{
    Q_UNUSED(maxItems);
    return {};
}

QList<Engine::HistoryEntry> WebkitPage::forwardItems(int maxItems) const
{
    Q_UNUSED(maxItems);
    return {};
}

void WebkitPage::goToHistoryEntry(const Engine::HistoryEntry &entry)
{
    Q_UNUSED(entry);
}

void WebkitPage::setZoomFactor(qreal factor)
{
    m_zoom = factor;
}

qreal WebkitPage::zoomFactor() const
{
    return m_zoom;
}

void WebkitPage::findText(const QString &subString,
                          Engine::FindFlags options)
{
    Q_UNUSED(subString);
    Q_UNUSED(options);
}

void WebkitPage::runJavaScript(
        const QString &source,
        const std::function<void(const QVariant &)> &resultCallback)
{
    Q_UNUSED(source);
    Q_UNUSED(resultCallback);
}

void WebkitPage::runJavaScriptLifted(
        const QString &source,
        const std::function<void(const QVariant &)> &resultCallback,
        quint32 worldId)
{
    Q_UNUSED(worldId);
    runJavaScript(source, resultCallback);
}

void WebkitPage::toHtml(
        const std::function<void(const QString &)> &resultCallback)
{
    if (resultCallback)
        resultCallback(QString());
}

QAction *WebkitPage::action(Engine::StandardAction action)
{
    Q_UNUSED(action);
    return nullptr;
}

bool WebkitPage::isLoading() const
{
    return false;
}

bool WebkitPage::recentlyAudible() const
{
    return false;
}

bool WebkitPage::isOffTheRecord() const
{
    return m_profile && m_profile->isOffTheRecord();
}

void WebkitPage::setPageAttribute(const QString &name, bool on)
{
    m_attributes.insert(name, on);
}

void WebkitPage::setLifecycleState(LifecycleState state)
{
    m_lifecycle = state;
}

WebkitPage::LifecycleState WebkitPage::lifecycleState() const
{
    return m_lifecycle;
}

qint64 WebkitPage::renderProcessId() const
{
    // No renderer-PID API exists in WPE WebKit 2.54 (named spike
    // gap — get_page_id is an internal PageID, not an OS pid).
    return -1;
}

void WebkitPage::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
}

void WebkitPage::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> WebkitPage::scripts() const
{
    return m_scripts.values();
}

void WebkitPage::download(const QUrl &url)
{
    Q_UNUSED(url);
}

QWidget *WebkitPage::view() const
{
    return m_view;
}

// ---- WebkitProfile --------------------------------------------------

WebkitProfile::WebkitProfile(const Engine::ProfileOptions &options,
                             QObject *parent)
    : Engine::Profile(parent)
    , m_options(options)
{
}

bool WebkitProfile::isOffTheRecord() const
{
    return m_options.offTheRecord;
}

QString WebkitProfile::storageName() const
{
    return m_options.storageName;
}

void WebkitProfile::setUserAgent(const QString &userAgent)
{
    m_userAgent = userAgent;
}

void WebkitProfile::setRequestPolicy(Engine::RequestPolicy *policy)
{
    m_policy = policy;
}

Engine::RequestPolicy *WebkitProfile::requestPolicy() const
{
    return m_policy;
}

void WebkitProfile::setCookieFilter(
        const std::function<bool(const Engine::CookieAttempt &)> &filter)
{
    m_cookieFilter = filter;
}

void WebkitProfile::insertScript(const Engine::Script &script)
{
    m_scripts.insert(script.name, script);
}

void WebkitProfile::removeScript(const QString &name)
{
    m_scripts.remove(name);
}

QList<Engine::Script> WebkitProfile::scripts() const
{
    return m_scripts.values();
}

void WebkitProfile::clear(Engine::StorageAreas areas)
{
    // WK12 maps this onto WebKitWebsiteDataManager::clear; nothing is
    // persisted before WK11 roots a session, so there is nothing to
    // wipe.
    Q_UNUSED(areas);
}

void WebkitProfile::setProfileAttribute(const QString &name,
                                        const QVariant &value)
{
    m_attributes.insert(name, value);
}

// ---- WebkitBackend --------------------------------------------------

WebkitBackend::WebkitBackend(QObject *parent)
    : Engine::Backend(parent)
{
}

bool WebkitBackend::librariesLinked()
{
#ifdef ARKTOS_WEBKIT_LINKED
    return true;
#else
    return false;
#endif
}

QString WebkitBackend::wpeVersion()
{
#ifdef ARKTOS_WEBKIT_LINKED
    // The call is also what keeps the DT_NEEDED on libWPEWebKit real
    // — an unreferenced link would let --as-needed drop the library
    // and make the gate's "links with libs present" claim vacuous.
    return QStringLiteral("%1.%2.%3")
            .arg(webkit_get_major_version())
            .arg(webkit_get_minor_version())
            .arg(webkit_get_micro_version());
#else
    return QString();
#endif
}

QString WebkitBackend::id() const
{
    return QStringLiteral("webkit");
}

QString WebkitBackend::displayName() const
{
    const QString version = wpeVersion();
    if (!version.isEmpty())
        return QStringLiteral("WebKit %1").arg(version);
    return QStringLiteral("WebKit");
}

QString WebkitBackend::engineNote() const
{
    return tr("Experimental engine — WPE WebKit backend (early)");
}

Engine::Capabilities WebkitBackend::capabilities() const
{
    // WK07: the conservative spike-verified table
    // (.devin/WEBKIT-SPIKE.md "Capabilities recommendations") — only
    // what WK01-WK04 proved or the 2.54 headers pin down.  The named
    // gaps stay off: no per-attempt CookieAttempt functor
    // (WebKitCookieManager offers only a 3-state accept policy), no
    // embedder devtools channel (the built-in inspector exists but
    // exports no attach API), WebKitWebExtension present-but-unprobed.
    // An unlinked TU reports all-false — nothing is faked.
    Engine::Capabilities caps;
    caps.downloads = false;
    if (!librariesLinked())
        return caps;
    caps.requestInterception = true;
    caps.scriptInjection = true;
    caps.scriptEvaluation = true;
    caps.perPageSettings = true;
    caps.lifecycleDiscard = true;
    caps.contextMenuInfo = true;
    caps.certificateOverride = true;
    caps.downloads = true;
    return caps;
}

bool WebkitBackend::initialize()
{
    // Direct linking IS the loadable probe — a libWPEWebKit that
    // failed to resolve would have stopped the process at exec.
    return librariesLinked();
}

Engine::Profile *WebkitBackend::createProfile(
        const Engine::ProfileOptions &options, QObject *parent)
{
    return new WebkitProfile(options, parent);
}

Engine::Page *WebkitBackend::createPage(Engine::Profile *, QObject *)
{
    // WK08 hosts a real page; until then the swap path refuses (the
    // caller falls back to Chromium) instead of manufacturing a tab
    // that can never render.
    return nullptr;
}

QString WebkitBackend::pageUnavailableReason() const
{
    if (!librariesLinked())
        return tr("the WPE WebKit libraries were not found at build "
                  "time");
    return tr("the WebKit backend cannot host pages yet");
}

QWidget *WebkitBackend::createView(Engine::Page *, QWidget *)
{
    return nullptr;
}

namespace {

// WK07: self-registration through the addProbe seam — the servo
// shape: this TU exists only in webkit-enabled objects while
// engineregistry.o is linked into subproject binaries (tools,
// autotests) that never see it.  The probe runs on the first
// EngineRegistry::backends() call; a compile-but-inert build
// (webkit=1, no pkg-config) fails initialize() and stays unlisted —
// the same "missing artifact hides" contract a servo-less host gets.
void probeWebkitBackend()
{
    static bool probed = false;
    if (probed)
        return;
    probed = true;
    // qApp-owned: the registry lists backends without owning them,
    // so an unparented registered backend would leak at exit.
    WebkitBackend *candidate = new WebkitBackend(qApp);
    if (candidate->initialize())
        EngineRegistry::registerBackend(candidate);
    else
        delete candidate;
}

struct WebkitProbeRegistration {
    WebkitProbeRegistration()
    {
        EngineRegistry::addProbe(&probeWebkitBackend);
    }
} webkitProbeRegistration;

} // namespace
