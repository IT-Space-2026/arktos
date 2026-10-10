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

#include "pdfsupport.h"

#include "browserpaths.h"
#include "webpage.h"

#ifdef ARORA_RUSTCORE
#include "rustcore.h"
#endif

#include <qdir.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qsettings.h>
#include <qtimer.h>
#include <quuid.h>
#include <qwebenginedownloadrequest.h>

// PDF magic may sit anywhere in the first KiB per the spec.
static bool looksLikePdfBytes(const QByteArray &data)
{
    return data.left(1024).contains("%PDF-");
}

// One-shot raw-view bypasses, keyed page -> url.  The pipeline's own
// fallback reload is consumed exactly once so a failed sanitize never
// loops the fetch, while a later user reload of the same url tries
// the pipeline again.
static QHash<quintptr, QSet<QString> > s_bypass;

namespace PdfSupport {

bool viewerEnabled()
{
    return QSettings().value(QLatin1String("privacy/pdfViewer"), true).toBool();
}

bool sanitizeEnabled()
{
#ifdef ARORA_RUSTCORE
    return QSettings().value(QLatin1String("privacy/pdfSanitize"), true).toBool();
#else
    return false;
#endif
}

bool looksLikePdfUrl(const QUrl &url)
{
    return url.path(QUrl::FullyDecoded)
        .endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive);
}

bool shouldIntercept(const QUrl &url, QWebEnginePage::NavigationType type)
{
    if (!viewerEnabled() || !sanitizeEnabled())
        return false;
    // A POST response cannot be re-fetched — the raw viewer path keeps
    // the response attached to its original request.
    if (type == QWebEnginePage::NavigationTypeFormSubmitted)
        return false;
    const QString scheme = url.scheme();
    if (scheme != QLatin1String("http")
            && scheme != QLatin1String("https")
            && scheme != QLatin1String("file"))
        return false;
    // The pipeline's own sanitized output passes straight through.
    if (url.isLocalFile() && isManagedPath(url.toLocalFile()))
        return false;
    return looksLikePdfUrl(url);
}

bool consumeBypass(WebPage *page, const QUrl &url)
{
    auto it = s_bypass.find(quintptr(page));
    if (it == s_bypass.end())
        return false;
    return it.value().remove(QString::fromUtf8(url.toEncoded()));
}

QByteArray sanitize(const QByteArray &input)
{
#ifdef ARORA_RUSTCORE
    if (input.isEmpty())
        return QByteArray();
    RcBuffer out = { nullptr, 0 };
    const RcStatus status = rc_pdf_sanitize(
        reinterpret_cast<const uint8_t *>(input.constData()),
        size_t(input.size()), &out);
    if (status != RC_OK || !out.data || out.len == 0)
        return QByteArray();
    const QByteArray cleaned(reinterpret_cast<const char *>(out.data),
                             int(out.len));
    rc_buffer_free(out);
    return cleaned;
#else
    Q_UNUSED(input);
    return QByteArray();
#endif
}

QString tempDirPath()
{
    const QString path = BrowserPaths::dataFilePath(QLatin1String("pdf-view"));
    QDir dir;
    if (dir.mkpath(path)) {
        QFile::setPermissions(path,
            QFileDevice::ReadOwner | QFileDevice::WriteOwner
                | QFileDevice::ExeOwner);
    }
    return path;
}

void sweepTempDir()
{
    const QDir dir(tempDirPath());
    const QStringList files = dir.entryList(QDir::Files | QDir::NoDotAndDotDot);
    for (const QString &name : files)
        QFile::remove(dir.filePath(name));
}

bool isManagedPath(const QString &localPath)
{
    return !localPath.isEmpty()
        && localPath.startsWith(tempDirPath() + QLatin1Char('/'));
}

bool managedViewerNavAllowed(const QUrl &current, const QUrl &target)
{
    if (!current.isLocalFile() || !isManagedPath(current.toLocalFile()))
        return true;
    if (target.isLocalFile() && isManagedPath(target.toLocalFile()))
        return true;
    const QString scheme = target.scheme();
    return scheme == QLatin1String("http")
        || scheme == QLatin1String("https");
}

} // namespace PdfSupport

QList<PdfSanitizeFetch *> PdfSanitizeFetch::s_pending;

PdfSanitizeFetch::PdfSanitizeFetch(WebPage *page, const QUrl &url,
                                   QObject *parent)
    : QObject(parent)
    , m_page(page)
    , m_url(url)
    , m_request(nullptr)
{
    // A dead fetch — claim never arrived or the transfer stalled —
    // hands the url back to the engine rather than stranding the tab.
    QTimer::singleShot(120 * 1000, this, &PdfSanitizeFetch::handleTimeout);
}

PdfSanitizeFetch::~PdfSanitizeFetch()
{
    s_pending.removeAll(this);
    if (m_request)
        m_request->cancel();
}

void PdfSanitizeFetch::start(WebPage *page, const QUrl &url)
{
    if (!page)
        return;

    if (url.isLocalFile()) {
        // Local PDFs need no fetch — read, rewrite, display.
        QFile in(url.toLocalFile());
        if (in.open(QIODevice::ReadOnly)) {
            const QByteArray raw = in.readAll();
            in.close();
            auto *fetch = new PdfSanitizeFetch(page, url, page);
            fetch->serveBytes(raw);
            return;
        }
        return;
    }

    auto *fetch = new PdfSanitizeFetch(page, url, page);
    s_pending.append(fetch);
    // The request lands on the profile's downloadRequested signal and
    // is claimed in DownloadManager before any DownloadItem is made —
    // profile cookies, SOCKS/Tor routing and container binding all
    // ride the page's own profile by construction.
    page->download(url);
}

bool PdfSanitizeFetch::claim(QWebEngineDownloadRequest *request)
{
    if (!request)
        return false;
    for (PdfSanitizeFetch *fetch : std::as_const(s_pending)) {
        if (fetch->m_request || !fetch->m_page
                || fetch->m_page != request->page()
                || fetch->m_url != request->url())
            continue;
        fetch->attach(request);
        return true;
    }
    return false;
}

void PdfSanitizeFetch::attach(QWebEngineDownloadRequest *request)
{
    m_request = request;
    m_rawPath = PdfSupport::tempDirPath() + QLatin1Char('/')
        + QUuid::createUuid().toString(QUuid::WithoutBraces)
        + QLatin1String(".raw");
    const QFileInfo info(m_rawPath);
    request->setDownloadDirectory(info.absolutePath());
    request->setDownloadFileName(info.fileName());
    connect(request, &QWebEngineDownloadRequest::stateChanged,
            this, &PdfSanitizeFetch::handleStateChanged);
    request->accept();
}

void PdfSanitizeFetch::handleStateChanged(
        QWebEngineDownloadRequest::DownloadState state)
{
    switch (state) {
    case QWebEngineDownloadRequest::DownloadCompleted:
        handleCompleted();
        break;
    case QWebEngineDownloadRequest::DownloadInterrupted:
    case QWebEngineDownloadRequest::DownloadCancelled:
        fail();
        break;
    default:
        break;
    }
}

void PdfSanitizeFetch::handleCompleted()
{
    QByteArray raw;
    if (m_request) {
        QFile in(m_request->downloadDirectory() + QLatin1Char('/')
                 + m_request->downloadFileName());
        if (in.open(QIODevice::ReadOnly))
            raw = in.readAll();
    }
    QFile::remove(m_rawPath);
    serveBytes(raw);
}

void PdfSanitizeFetch::serveBytes(const QByteArray &raw)
{
    // Only genuine PDFs go through the rewrite — a .pdf url serving
    // anything else (an error page, a redirect landing) takes the
    // documented raw fallback and renders as its true type.
    if (looksLikePdfBytes(raw)) {
        const QByteArray cleaned = PdfSupport::sanitize(raw);
        if (!cleaned.isEmpty()) {
            const QString outPath = PdfSupport::tempDirPath()
                + QLatin1Char('/')
                + QUuid::createUuid().toString(QUuid::WithoutBraces)
                + QLatin1String(".pdf");
            QFile out(outPath);
            if (out.open(QIODevice::WriteOnly)) {
                out.write(cleaned);
                out.close();
                QFile::setPermissions(outPath,
                    QFileDevice::ReadOwner | QFileDevice::WriteOwner);
                if (m_page)
                    m_page->load(QUrl::fromLocalFile(outPath));
                dispose();
                return;
            }
        }
    }
    fail();
}

void PdfSanitizeFetch::fail()
{
    if (m_page) {
        s_bypass[quintptr(m_page.data())].insert(
            QString::fromUtf8(m_url.toEncoded()));
        m_page->load(m_url);
    }
    dispose();
}

void PdfSanitizeFetch::handleTimeout()
{
    fail();
}

void PdfSanitizeFetch::dispose()
{
    s_pending.removeAll(this);
    m_request = nullptr;
    deleteLater();
}
