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

SourceViewer::SourceViewer(const QString &source, const QString &title,
                           const QUrl &url, QWidget *parent)
    : QDialog(parent)
    , m_edit(new QPlainTextEdit(tr("Loading..."), this))
    , m_highlighter(new SourceHighlighter(m_edit->document()))
    , m_plainTextEditSearch(new PlainTextEditSearch(m_edit, this))
    , m_layout(new QVBoxLayout(this))
    , m_menuBar(new QMenuBar(this))
    , m_editMenu(new QMenu(tr("&Edit"), m_menuBar))
    , m_findAction(new QAction(tr("&Find"), m_editMenu))
    , m_reply(nullptr)
    , m_source(source)
{
    setWindowTitle(tr("Source of Page %1").arg(title));

    QSettings settings;
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
    m_reply->setParent(this);
}

SourceViewer::~SourceViewer()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("SourceViewer"));
    settings.setValue(QLatin1String("size"), size());
}

void SourceViewer::loadingFinished()
{
    const QByteArray response = m_reply->readAll();
    const QUrl url = m_reply->request().url();
    const bool failed = m_reply->error() != QNetworkReply::NoError;
    // Qt 6's setContent treats an empty mime type as text/plain;
    // without a reply Content-Type assume the page was HTML.
    QString mimeType =
        m_reply->header(QNetworkRequest::ContentTypeHeader).toString()
            .section(QLatin1Char(';'), 0, 0);
    if (mimeType.isEmpty())
        mimeType = QLatin1String("text/html");
    m_reply->close();
    m_reply->deleteLater();
    m_reply = nullptr;

    if (failed) {
        m_edit->setPlainText(m_source);
        return;
    }

    /* If the raw bytes parse to the same DOM the page reported, show
       the pristine wire source; otherwise (POST result, a document the
       page rewrote...) fall back to the DOM serialization the caller
       passed in.  WebEngine has no synchronous HTML parser like
       QWebFrame::setContent, so the comparison runs on a throwaway
       page asynchronously. */
    QWebEnginePage *probe = new QWebEnginePage(this);
    // The probe is engine-bound infra (a throwaway DOM parser) — its
    // serialization still goes through the interface surface.
    WebEnginePageAdapter *probeAdapter = new WebEnginePageAdapter(probe, probe);
    QPointer<SourceViewer> self(this);
    connect(probe, &QWebEnginePage::loadFinished, this,
            [self, probe, probeAdapter, response](bool ok) {
        if (!ok) {
            self->m_edit->setPlainText(self->m_source);
            probe->deleteLater();
            return;
        }
        probeAdapter->toHtml([self, probe, response](const QString &markup) {
            if (self) {
                self->m_edit->setPlainText(
                    markup == self->m_source
                        ? QString::fromUtf8(response)
                        : self->m_source);
            }
            delete probe;
        });
    });
    // If the probe never reports back (setContent can silently fail),
    // the DOM dump is a valid fallback.
    QPointer<QWebEnginePage> probeGuard(probe);
    QTimer::singleShot(5000, this, [self, probeGuard]() {
        if (self && probeGuard) {
            self->m_edit->setPlainText(self->m_source);
            delete probeGuard.data();
        }
    });
    probe->setContent(response, mimeType, url);
}
