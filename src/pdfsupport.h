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

#ifndef PDFSUPPORT_H
#define PDFSUPPORT_H

#include <qbytearray.h>
#include <qobject.h>
#include <qpointer.h>
#include <qurl.h>
#include <qwebenginepage.h>

class QWebEngineDownloadRequest;
class WebPage;

// PDF01: the in-browser PDF pipeline.
//
// QtWebEngine ships Chromium's PDFium viewer behind
// QWebEngineSettings::PdfViewerEnabled (applied per profile in
// BrowserProfile::applySettings, bound to privacy/pdfViewer).
// On top of that, the sanitize-on-view pipeline owns .pdf
// navigations when privacy/pdfSanitize is on: the navigation is
// refused in WebPage::acceptNavigationRequest, the bytes are
// fetched through the page's own download machinery (profile
// cookies, SOCKS/Tor routing and container binding ride along),
// rewritten by the rustcore policy pass and displayed from the
// managed temp dir — the viewer never sees remote bytes.
//
// Deliberately NOT covered:
//   * the download path — a "save as" PDF keeps raw bytes for the
//     user's external reader (sanitize applies to viewing only);
//   * MIME-sniffed PDFs on non-.pdf URLs — the engine renders those
//     raw through the viewer; interception needs a response header
//     the embedder API does not expose pre-body;
//   * POST responses — a re-fetch cannot replay the body.
//
// On fetch or sanitizer failure the pipeline records a one-shot
// bypass and loads the original URL raw — a malformed "PDF" then
// renders as its true type rather than dead-ending the tab.
namespace PdfSupport {

// privacy/pdfViewer — Chromium's built-in PDFium viewer, default on.
bool viewerEnabled();
// privacy/pdfSanitize — the pre-view rewrite, default on.  Builds
// without the rust core have no sanitizer; the toggle reads false
// there and viewing falls back to the raw viewer path.
bool sanitizeEnabled();
// The url's decoded path ends in ".pdf" (query/fragment ignored).
bool looksLikePdfUrl(const QUrl &url);
// Whether this main-frame navigation should divert into the
// fetch -> sanitize -> display pipeline.
bool shouldIntercept(const QUrl &url, QWebEnginePage::NavigationType type);
// Consumes a recorded one-shot raw-view bypass for (page, url) —
// true when this navigation is the pipeline's own fallback reload.
bool consumeBypass(WebPage *page, const QUrl &url);
// Raw body in, sanitized standalone PDF out; empty on failure.
QByteArray sanitize(const QByteArray &input);
// <data dir>/pdf-view — 0700, swept at startup.
QString tempDirPath();
void sweepTempDir();
// localPath lives inside the managed temp dir.
bool isManagedPath(const QString &localPath);
// The staged viewer document is file://-origin, so a hostile pdf's
// embedded file:///etc/passwd-style link would read as a
// local-to-local navigation Chromium has no reason to refuse.  This
// rule is the clamp: while a staged copy is displayed only remote
// http(s) targets and other managed-dir files may navigate.
bool managedViewerNavAllowed(const QUrl &current, const QUrl &target);

} // namespace PdfSupport

// PdfSanitizeFetch — the async half of the pipeline: one object per
// refused .pdf navigation, parented to the page so a closing tab
// cancels its fetch.  The request is initiated with
// QWebEnginePage::download() and claimed back out of
// DownloadManager::handleDownloadRequested before any DownloadItem
// is made — no UI surface ever shows the staging write.
class PdfSanitizeFetch : public QObject
{
    Q_OBJECT

public:
    static void start(WebPage *page, const QUrl &url);
    // Claimed by DownloadManager before external-handler, retry and
    // engine routing — matched on the requesting page + url.
    static bool claim(QWebEngineDownloadRequest *request);

private slots:
    void handleStateChanged(QWebEngineDownloadRequest::DownloadState state);
    void handleTimeout();

private:
    PdfSanitizeFetch(WebPage *page, const QUrl &url, QObject *parent);
    ~PdfSanitizeFetch() override;

    void attach(QWebEngineDownloadRequest *request);
    void handleCompleted();
    void serveBytes(const QByteArray &raw);
    // Records the one-shot bypass and reloads the original url raw.
    void fail();
    void dispose();

    static QList<PdfSanitizeFetch *> s_pending;

    QPointer<WebPage> m_page;
    QUrl m_url;
    QWebEngineDownloadRequest *m_request;
    QString m_rawPath;
};

#endif // PDFSUPPORT_H
