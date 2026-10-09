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

#include "rustdownloadengine.h"

#include "browserpaths.h"

#include <qjsonobject.h>
#include <qjsondocument.h>
#include <qsettings.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

/*!
    Poll cadence: the spec's ~2 progress updates per second.  The FFI
    poll itself is a handful of atomic reads — cheap enough that the
    timer staying this fast while RUNNING is fine.
 */
static const int s_pollIntervalMs = 500;

RustDownloadEngine::RustDownloadEngine(QWebEnginePage *page, const QUrl &url,
                                       const QString &suggestedFileName,
                                       const QString &mimeType, QObject *parent)
    : QObject(parent)
    , m_handle(0)
    , m_page(page)
    , m_url(url)
    , m_suggested(suggestedFileName)
    , m_mimeType(mimeType)
    , m_state(QWebEngineDownloadRequest::DownloadRequested)
    , m_received(0)
    , m_total(-1)
{
    m_timer.setInterval(s_pollIntervalMs);
    connect(&m_timer, &QTimer::timeout, this, &RustDownloadEngine::poll);
}

RustDownloadEngine::~RustDownloadEngine()
{
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
    }
}

bool RustDownloadEngine::isAvailable()
{
    return dl_is_available() != 0;
}

bool RustDownloadEngine::isSelected()
{
    if (!isAvailable())
        return false;
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    const QVariant v = settings.value(QLatin1String("engine"));
    const QString s = v.toString().toLower();
    return s.contains(QLatin1String("rust"))
        || s.contains(QLatin1String("accelerated"))
        || v.toInt() == 1;
}

void RustDownloadEngine::accept()
{
    if (m_handle || m_dir.isEmpty())
        return;

    // Part files live under the app data dir (0700), never next to
    // user-visible content until the atomic rename lands the result.
    static bool tempArmed = false;
    if (!tempArmed) {
        const QByteArray p = BrowserPaths::dataFilePath(
            QLatin1String("downloads-parts")).toUtf8();
        if (dl_set_temp_dir(p.constData()) != DL_OK) {
            finish(QWebEngineDownloadRequest::DownloadInterrupted,
                   tr("Download engine temp directory unavailable"));
            return;
        }
        tempArmed = true;
    }

    // User-configurable 4-16 segments (0/empty = engine default 8);
    // the DLACC01 settings page owns the visible control.
    QSettings settings;
    settings.beginGroup(QLatin1String("downloadmanager"));
    const int connections = settings.value(QLatin1String("connections"), 0).toInt();

    // Header parity seed (DLACC04 owns the full policy): the engine
    // presents the profile's UA so downloads are not a fingerprint
    // diff.  No cookies are exported yet — DLACC05 wires the per-host
    // Netscape file; until then authenticated sites fall back
    // naturally on the server side (403 -> Interrupted, retryable).
    QJsonObject options;
    if (m_page && m_page->profile()) {
        const QString ua = m_page->profile()->httpUserAgent();
        if (!ua.isEmpty())
            options.insert(QLatin1String("user_agent"), ua);
    }

    const QByteArray u = m_url.toString().toUtf8();
    const QByteArray d = m_dir.toUtf8();
    const QByteArray n = m_fileName.toUtf8();
    const QByteArray o = QJsonDocument(options).toJson(QJsonDocument::Compact);
    DlHandle handle = 0;
    const DlStatus st = dl_start(u.constData(), d.constData(),
                                 n.isEmpty() ? nullptr : n.constData(),
                                 connections, nullptr, o.constData(),
                                 &handle);
    if (st != DL_OK) {
        char *msg = dl_last_error_message();
        const QString reason = msg ? QString::fromUtf8(msg) : QString();
        if (msg)
            dl_string_free(msg);
        finish(QWebEngineDownloadRequest::DownloadInterrupted,
               reason.isEmpty() ? tr("Download engine failed to start") : reason);
        return;
    }
    m_handle = handle;
    m_received = 0;
    m_total = -1;
    m_state = QWebEngineDownloadRequest::DownloadInProgress;
    emit stateChanged(m_state);
    m_timer.start();
    poll();
}

void RustDownloadEngine::cancel()
{
    if (m_handle) {
        dl_cancel(m_handle);
    } else {
        // No handle yet (cancelled at the filename prompt) — report
        // the transition directly so the item finishes.
        finish(QWebEngineDownloadRequest::DownloadCancelled);
    }
}

void RustDownloadEngine::restart()
{
    if (m_handle) {
        dl_cancel(m_handle);
        dl_free(m_handle);
        m_handle = 0;
    }
    m_timer.stop();
    m_received = 0;
    m_total = -1;
    m_error.clear();
    m_output.clear();
    m_state = QWebEngineDownloadRequest::DownloadRequested;
    emit stateChanged(m_state);
}

bool RustDownloadEngine::isFinished() const
{
    switch (m_state) {
    case QWebEngineDownloadRequest::DownloadCompleted:
    case QWebEngineDownloadRequest::DownloadCancelled:
    case QWebEngineDownloadRequest::DownloadInterrupted:
        return true;
    default:
        return false;
    }
}

void RustDownloadEngine::poll()
{
    if (!m_handle)
        return;

    DlProgress p;
    if (dl_poll(m_handle, &p) != DL_OK)
        return;

    const qint64 received = p.bytes_done;
    const qint64 total = p.bytes_total;
    if (received != m_received) {
        m_received = received;
        emit receivedBytesChanged();
    }
    if (total != m_total) {
        m_total = total;
        emit totalBytesChanged();
    }

    switch (static_cast<DlState>(p.state)) {
    case DL_PROBING:
    case DL_RUNNING:
    case DL_MERGING:
        break;
    case DL_DONE: {
        char *out = dl_output_path(m_handle);
        if (out) {
            m_output = QString::fromUtf8(out);
            dl_string_free(out);
        }
        finish(QWebEngineDownloadRequest::DownloadCompleted);
        break;
    }
    case DL_FAILED: {
        char *msg = dl_error_message(m_handle);
        const QString reason = msg ? QString::fromUtf8(msg) : QString();
        if (msg)
            dl_string_free(msg);
        finish(QWebEngineDownloadRequest::DownloadInterrupted,
               reason.isEmpty() ? tr("Download interrupted") : reason);
        break;
    }
    case DL_CANCELLED:
        finish(QWebEngineDownloadRequest::DownloadCancelled);
        break;
    }
}

void RustDownloadEngine::finish(QWebEngineDownloadRequest::DownloadState state,
                                const QString &error)
{
    if (isFinished())
        return;
    m_timer.stop();
    m_error = error;
    m_state = state;
    emit stateChanged(m_state);
}
