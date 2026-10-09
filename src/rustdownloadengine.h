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

#ifndef RUSTDOWNLOADENGINE_H
#define RUSTDOWNLOADENGINE_H

#include <qpointer.h>
#include <qtimer.h>
#include <qurl.h>
#include <qwebenginedownloadrequest.h>
#include <qwebenginepage.h>

#include "rustdl.h"

/*!
    RustDownloadEngine is the Qt-side face of the rustdl crate
    (DLACC03/DLACC06): one object owns one dl_start handle and polls it
    on a ~2 Hz timer, emitting the same signal surface a
    QWebEngineDownloadRequest offers so a DownloadItem card can show
    either backend unchanged.

    States are reported AS QWebEngineDownloadRequest::DownloadState so
    DownloadItem's existing slot handles both engines: the crate's
    PROBING/RUNNING/MERGING all read as DownloadInProgress, DONE as
    DownloadCompleted, FAILED as DownloadInterrupted, CANCELLED as
    DownloadCancelled.

    Only built under CONFIG+=rustdl; without it nothing here exists
    and the engine selector (DLACC01) never offers "Accelerated".
 */
class RustDownloadEngine : public QObject
{
    Q_OBJECT

public:
    // A download taken over from a QWebEngineDownloadRequest (the
    // downloadRequested path) — the request's page/url/suggested
    // name/mime are copied, Chromium's own fetch is cancelled by the
    // caller.
    RustDownloadEngine(QWebEnginePage *page, const QUrl &url,
                       const QString &suggestedFileName,
                       const QString &mimeType, QObject *parent = nullptr);
    ~RustDownloadEngine() override;

    // Reads the downloadmanager/engine setting the DLACC01 selector
    // page writes.  Lenient on the value's shape ("rust",
    // "accelerated", "Accelerated (Rust)", 1) so either side can land
    // first; the selector only shows the option when isAvailable().
    static bool isSelected();
    // Crate linked in.  (Temp dir is armed lazily at accept() — the
    // engine reports available whenever the FFI is present.)
    static bool isAvailable();

    // Whether this engine may fetch `url` at all (DLACC04).  Scheme
    // must be http(s) — anything else stays with the normal engine.
    // In a tor process the hard rule applies: the download exits via
    // the managed SOCKS proxy or it does not run on this engine —
    // callers fall back to the engine-mediated path rather than let a
    // download ride a direct connection.
    static bool canHandle(const QUrl &url);

    // The DLACC04 policy gate, exposed as a public static so the
    // autotest can probe the decisions directly.  gateCheck() is the
    // pure verdict the FFI trampoline (dl_set_gate) hands the Rust
    // worker threads: the same interceptor/adblock/blocklist policy
    // the normal engine applies, re-evaluated per redirect hop.
    // `network` is normally AdBlockManager::instance()->network();
    // null skips the adblock step.
    enum GateAction { GateAllow = 0, GateBlock = 1, GateRewrite = 2 };
    struct GateDecision {
        GateAction action = GateAllow;
        QString url;    // GateRewrite payload
        QString reason; // GateBlock payload (host-bearing, query-free)
    };
    static GateDecision gateCheck(const QUrl &url, const QUrl &prev,
                                  const QUrl &firstParty,
                                  const QString &scope,
                                  class AdBlockNetwork *network);

    // The QWebEngineDownloadRequest-shaped surface DownloadItem uses.
    QUrl url() const { return m_url; }
    QWebEnginePage *page() const { return m_page; }
    QString mimeType() const { return m_mimeType; }
    QString suggestedFileName() const { return m_suggested; }
    qint64 receivedBytes() const { return m_received; }
    qint64 totalBytes() const { return m_total; }
    QWebEngineDownloadRequest::DownloadState state() const { return m_state; }
    bool isFinished() const;
    QString interruptReasonString() const { return m_error; }

    // Mirrors the request's setDownloadDirectory/setDownloadFileName/
    // accept contract: the item picks a destination, then accept()
    // issues dl_start.  cancel() maps to dl_cancel; restart() resets
    // the object so the item's normal filename/accept flow re-runs
    // (Try Again).
    void setDownloadDirectory(const QString &dir) { m_dir = dir; }
    void setDownloadFileName(const QString &name) { m_fileName = name; }
    void accept();
    void cancel();
    void restart();

    // Final on-disk path once state() is DownloadCompleted.
    QString outputPath() const { return m_output; }

signals:
    void stateChanged(QWebEngineDownloadRequest::DownloadState state);
    void receivedBytesChanged();
    void totalBytesChanged();

private slots:
    void poll();

private:
    void finish(QWebEngineDownloadRequest::DownloadState state,
                const QString &error = QString());

    DlHandle m_handle;
    QPointer<QWebEnginePage> m_page;
    QUrl m_url;
    QString m_suggested;
    QString m_mimeType;
    QString m_dir;
    QString m_fileName;
    QString m_error;
    QString m_output;
    QWebEngineDownloadRequest::DownloadState m_state;
    qint64 m_received;
    qint64 m_total;
    QTimer m_timer;
};

#endif // RUSTDOWNLOADENGINE_H
