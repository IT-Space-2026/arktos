/*
 * Copyright 2008 Christian Franke <cfchris6@ts2server.com>
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

#include "sourceviewer.h"
#include "profilesettings.h"

#include <qdebug.h>
#include <qlayout.h>
#include <qmenubar.h>
#include <qnetworkcookie.h>
#include <qnetworkreply.h>
#include <qnetworkrequest.h>
#include <qplaintextedit.h>
#include <qpointer.h>
#include <qshortcut.h>
#include <qsettings.h>
#include <qtimer.h>
#include <qwebenginepage.h>

#include "networkaccessmanager.h"
#include "plaintexteditsearch.h"
#include "sourcehighlighter.h"
#include "webenginebackend.h"

namespace {

/* A fetch that hangs used to hold the viewer open forever — give up
   and let the DOM serialization answer instead.  The env override
   keeps the failure path testable without a fifteen-second fixture
   stall. */
qint64 fetchTimeoutMs()
{
    bool ok = false;
    const qint64 overrideMs = qEnvironmentVariableIntValue(
        "ARORA_SOURCE_FETCH_TIMEOUT_MS", &ok);
    return ok && overrideMs > 0 ? overrideMs : 15000;
}

/* The wire-vs-DOM probe spawns a throwaway engine page whose
   setContent + parse + re-serialize can cost seconds on multi-
   megabyte documents, so pristine wire bytes win outright once the
   payload is large.  Smaller HTML still needs the probe: a mutated
   document (POST result, scripted rewrite) must not be replaced by
   stale wire source. */
const qint64 kProbeMaxBytes = 4 * 1024 * 1024;

bool isHtmlMime(const QString &mime)
{
    return mime == QLatin1String("text/html")
        || mime == QLatin1String("application/xhtml+xml");
}

/* A non-HTML reply needs no probe — text bytes are their own source
   (json/xml/css/plain), while a NUL-pocked payload is binary and the
   DOM dump (the image/object document) reads better. */
bool looksLikeText(const QByteArray &bytes)
{
    return !bytes.left(4096).contains('\0');
}

bool timingEnabled()
{
    static const bool enabled =
        qEnvironmentVariableIsSet("ARORA_SOURCE_TIMING");
    return enabled;
}

}

SourceViewer::SourceViewer(const QString &title, const QUrl &url,
                           QWidget *parent)
    : QDialog(parent)
    , m_edit(new QPlainTextEdit(tr("Fetching source..."), this))
    , m_highlighter(new SourceHighlighter(m_edit->document()))
    , m_plainTextEditSearch(new PlainTextEditSearch(m_edit, this))
    , m_layout(new QVBoxLayout(this))
    , m_menuBar(new QMenuBar(this))
    , m_editMenu(new QMenu(tr("&Edit"), m_menuBar))
    , m_findAction(new QAction(tr("&Find"), m_editMenu))
    , m_reply(nullptr)
    , m_fetchDone(false)
    , m_fetchOk(false)
    , m_domDone(false)
    , m_resolved(false)
    , m_fetchMs(-1)
    , m_domMs(-1)
    , m_probeStartMs(-1)
{
    setWindowTitle(tr("Source of Page %1").arg(title));

    ProfileSettings settings;
    settings.beginGroup(QLatin1String("SourceViewer"));
    QSize size = settings.value(QLatin1String("size"), QSize(640, 480)).toSize();
    resize(size);

    m_edit->setLineWrapMode(QPlainTextEdit::WidgetWidth);
    m_edit->setReadOnly(true);
    QFont font = m_edit->font();
    font.setFamily(QLatin1String("Monospace"));
    m_edit->setFont(font);
    m_edit->setLineWidth(0);
    m_edit->setFrameShape(QFrame::NoFrame);

    m_menuBar->addMenu(m_editMenu);
    m_editMenu->addAction(m_findAction);
    m_findAction->setShortcuts(QKeySequence::Find);
    connect(m_findAction, &QAction::triggered,
            m_plainTextEditSearch, &SearchBar::showFind);

    m_layout->setSpacing(0);
    m_layout->setContentsMargins(0, 0, 0, 0);
    m_layout->addWidget(m_menuBar);
    m_layout->addWidget(m_plainTextEditSearch);
    m_layout->addWidget(m_edit);
    setLayout(m_layout);

    // Web page traffic belongs to Chromium, but this re-fetch is an
    // application-side GET so it goes through the app's NAM (proxy,
    // disk cache, Accept-Language) exactly like the WebKit version did.
    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::CacheLoadControlAttribute, QNetworkRequest::PreferCache);
    m_reply = NetworkAccessManager::instance()->get(request);
    connect(m_reply, &QNetworkReply::finished, this, &SourceViewer::loadingFinished);
    connect(m_reply, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
        if (m_resolved || m_fetchDone)
            return;
        if (total > 0)
            m_edit->setPlainText(
                tr("Fetching source... %1%").arg(received * 100 / total));
        else
            m_edit->setPlainText(
                tr("Fetching source... %1 KB").arg(received / 1024));
    });
    m_reply->setParent(this);
    m_clock.start();

    QPointer<SourceViewer> self(this);
    const qint64 timeout = fetchTimeoutMs();
    QTimer::singleShot(timeout, this, [self]() {
        if (self && self->m_reply)
            self->m_reply->abort();  // finished() reports the failure
    });
    // If neither stage answers (the page died before its toHtml
    // returned) surface whatever arrived instead of loading forever.
    QTimer::singleShot(timeout + 15000, this, [self]() {
        if (!self || self->m_resolved)
            return;
        if (self->m_fetchDone && self->m_fetchOk
            && looksLikeText(self->m_wire)) {
            self->showSource(QString::fromUtf8(self->m_wire),
                             "watchdog/wire");
        } else if (self->m_domDone) {
            self->showSource(self->m_source, "watchdog/dom");
        } else {
            self->showSource(tr("Source unavailable."), "watchdog/none");
        }
    });
}

SourceViewer::~SourceViewer()
{
    ProfileSettings settings;
    settings.beginGroup(QLatin1String("SourceViewer"));
    settings.setValue(QLatin1String("size"), size());
}

bool SourceViewer::isResolved() const
{
    return m_resolved;
}

void SourceViewer::setDomSource(const QString &markup)
{
    if (m_domDone || m_resolved)
        return;
    m_source = markup;
    m_domDone = true;
    m_domMs = m_clock.elapsed();
    maybeResolve();
}

void SourceViewer::loadingFinished()
{
    m_wire = m_reply->readAll();
    m_wireUrl = m_reply->request().url();
    m_fetchOk = m_reply->error() == QNetworkReply::NoError;
    // Qt 6's setContent treats an empty mime type as text/plain;
    // without a reply Content-Type assume the page was HTML.
    m_wireMime =
        m_reply->header(QNetworkRequest::ContentTypeHeader).toString()
            .section(QLatin1Char(';'), 0, 0);
    if (m_wireMime.isEmpty())
        m_wireMime = QLatin1String("text/html");
    m_reply->close();
    m_reply->deleteLater();
    m_reply = nullptr;
    m_fetchDone = true;
    m_fetchMs = m_clock.elapsed();
    if (!m_fetchOk && !m_domDone)
        m_edit->setPlainText(tr("Reading page DOM..."));
    maybeResolve();
}

void SourceViewer::maybeResolve()
{
    if (m_resolved || !m_fetchDone || !m_domDone)
        return;

    if (!m_fetchOk) {
        showSource(m_source, "fetch-failed/dom");
        return;
    }
    if (m_wire == m_source.toUtf8()) {
        showSource(QString::fromUtf8(m_wire), "exact-match/wire");
        return;
    }
    // The probe only runs where it can change the answer.
    if (isHtmlMime(m_wireMime) && m_wire.size() <= kProbeMaxBytes) {
        probeWireSource();
        return;
    }
    const bool text = looksLikeText(m_wire);
    showSource(text ? QString::fromUtf8(m_wire) : m_source,
               text ? "text/wire" : "binary/dom");
}

void SourceViewer::showSource(const QString &text, const char *decision)
{
    m_resolved = true;
    m_edit->setPlainText(text);
    if (timingEnabled()) {
        const qint64 total = m_clock.elapsed();
        qInfo() << "SourceViewer timing: decision" << decision
                << "fetch" << m_fetchMs << "ms"
                << "dom" << m_domMs << "ms"
                << "probe"
                << (m_probeStartMs >= 0 ? total - m_probeStartMs : -1)
                << "ms total" << total << "ms";
    }
}

void SourceViewer::probeWireSource()
{
    /* If the raw bytes parse to the same DOM the page reported, show
       the pristine wire source; otherwise (POST result, a document the
       page rewrote...) fall back to the DOM serialization the caller
       passed in.  WebEngine has no synchronous HTML parser like
       QWebFrame::setContent, so the comparison runs on a throwaway
       page asynchronously. */
    m_probeStartMs = m_clock.elapsed();
    QWebEnginePage *probe = new QWebEnginePage(this);
    // The probe is engine-bound infra (a throwaway DOM parser) — its
    // serialization still goes through the interface surface.
    WebEnginePageAdapter *probeAdapter = new WebEnginePageAdapter(probe, probe);
    QPointer<SourceViewer> self(this);
    connect(probe, &QWebEnginePage::loadFinished, this,
            [self, probe, probeAdapter](bool ok) {
        if (!self || self->m_resolved) {
            probe->deleteLater();
            return;
        }
        if (!ok) {
            self->showSource(self->m_source, "probe-failed/dom");
            probe->deleteLater();
            return;
        }
        probeAdapter->toHtml([self, probe](const QString &markup) {
            if (self && !self->m_resolved) {
                const bool same = markup == self->m_source;
                self->showSource(same
                                     ? QString::fromUtf8(self->m_wire)
                                     : self->m_source,
                                 same ? "probe-same/wire"
                                      : "probe-diff/dom");
            }
            delete probe;
        });
    });
    // If the probe never reports back (setContent can silently fail),
    // the DOM dump is a valid fallback.
    QPointer<QWebEnginePage> probeGuard(probe);
    QTimer::singleShot(5000, this, [self, probeGuard]() {
        if (self && !self->m_resolved && probeGuard) {
            self->showSource(self->m_source, "probe-timeout/dom");
            delete probeGuard.data();
        }
    });
    probe->setContent(m_wire, m_wireMime, m_wireUrl);
}
