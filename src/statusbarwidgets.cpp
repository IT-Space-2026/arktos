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
#include "statusbarwidgets.h"

#include "downloadmanager.h"
#include "webview.h"
#include "utils/aroraicon.h"

#include <qboxlayout.h>
#include <qcoreapplication.h>
#include <qdir.h>
#include <qfile.h>
#include <qlabel.h>
#include <qprogressbar.h>
#include <qtimer.h>
#include <qtoolbutton.h>
#include <qwebenginepage.h>
#include <qwebengineview.h>

LoadingIndicator::LoadingIndicator(QWidget *parent)
    : QWidget(parent)
    , m_bar(new QProgressBar(this))
    , m_label(new QLabel(this))
    , m_tick(new QTimer(this))
    , m_clearTimer(new QTimer(this))
    , m_loading(false)
    , m_lastElapsedMs(0)
{
    setObjectName(QLatin1String("loadingIndicator"));
    m_bar->setObjectName(QLatin1String("loadingBar"));
    m_label->setObjectName(QLatin1String("loadingLabel"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(4);
    m_bar->setRange(0, 100);
    m_bar->setTextVisible(false);
    m_bar->setFixedSize(72, 10);
    layout->addWidget(m_bar);
    layout->addWidget(m_label);

    m_tick->setInterval(250);
    connect(m_tick, &QTimer::timeout,
            this, &LoadingIndicator::updateElapsed);
    m_clearTimer->setSingleShot(true);
    m_clearTimer->setInterval(5000);
    connect(m_clearTimer, &QTimer::timeout,
            this, &LoadingIndicator::clearStatus);

    setVisible(false);
}

WebView *LoadingIndicator::webView() const
{
    return m_view;
}

void LoadingIndicator::setWebView(WebView *view)
{
    if (m_view == view)
        return;
    if (m_view)
        m_view->disconnect(this);
    m_view = view;
    m_loading = false;
    m_tick->stop();
    m_clearTimer->stop();
    setVisible(false);
    if (!m_view)
        return;
    connect(m_view, &QWebEngineView::loadStarted,
            this, &LoadingIndicator::pageLoadStarted);
    connect(m_view, &QWebEngineView::loadProgress,
            this, &LoadingIndicator::pageLoadProgress);
    connect(m_view, &QWebEngineView::loadFinished,
            this, &LoadingIndicator::pageLoadFinished);
    // Attaching mid-load (tab switch during a load) — the elapsed
    // timer can only start from here, so that page under-reports.
    const int progress = m_view->progress();
    if (progress > 0 && progress < 100)
        pageLoadStarted();
}

void LoadingIndicator::pageLoadStarted()
{
    m_loading = true;
    m_lastElapsedMs = 0;
    m_elapsed.start();
    m_clearTimer->stop();
    m_bar->setValue(0);
    m_bar->setVisible(true);
    m_label->setText(QLatin1String("0.0 s"));
    setVisible(true);
    m_tick->start();
}

void LoadingIndicator::pageLoadProgress(int progress)
{
    m_bar->setValue(progress);
    updateElapsed();
}

void LoadingIndicator::pageLoadFinished(bool ok)
{
    m_lastElapsedMs = m_elapsed.isValid() ? m_elapsed.elapsed() : 0;
    m_loading = false;
    m_tick->stop();
    m_bar->setVisible(false);
    const QString elapsed =
        QString::number(m_lastElapsedMs / 1000.0, 'f', 1);
    m_label->setText(ok ? tr("Loaded in %1 s").arg(elapsed)
                        : tr("Load failed after %1 s").arg(elapsed));
    setVisible(true);
    m_clearTimer->start();
}

void LoadingIndicator::updateElapsed()
{
    if (!m_elapsed.isValid())
        return;
    m_label->setText(QString::number(m_elapsed.elapsed() / 1000.0, 'f', 1)
                     + QLatin1String(" s"));
}

void LoadingIndicator::clearStatus()
{
    setVisible(false);
}

ZoomControl::ZoomControl(QWidget *parent)
    : QWidget(parent)
    , m_outButton(new QToolButton(this))
    , m_valueButton(new QToolButton(this))
    , m_inButton(new QToolButton(this))
{
    setObjectName(QLatin1String("zoomControl"));
    m_outButton->setObjectName(QLatin1String("zoomOutButton"));
    m_valueButton->setObjectName(QLatin1String("zoomValueButton"));
    m_inButton->setObjectName(QLatin1String("zoomInButton"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_outButton);
    layout->addWidget(m_valueButton);
    layout->addWidget(m_inButton);

    m_outButton->setIcon(AroraIcon::get(QLatin1String("zoom-out")));
    m_outButton->setAutoRaise(true);
    m_outButton->setIconSize(QSize(14, 14));
    m_outButton->setToolTip(tr("Zoom Out"));
    m_valueButton->setToolButtonStyle(Qt::ToolButtonTextOnly);
    m_valueButton->setAutoRaise(true);
    m_valueButton->setToolTip(tr("Reset Zoom"));
    // Pin the width to the largest ladder entry so the percent does
    // not jitter the status bar on every zoom step.
    m_valueButton->setMinimumWidth(
        fontMetrics().horizontalAdvance(QLatin1String("300%")) + 12);
    m_inButton->setIcon(AroraIcon::get(QLatin1String("zoom-in")));
    m_inButton->setAutoRaise(true);
    m_inButton->setIconSize(QSize(14, 14));
    m_inButton->setToolTip(tr("Zoom In"));

    connect(m_outButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->zoomOut();
    });
    connect(m_valueButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->resetZoom();
    });
    connect(m_inButton, &QToolButton::clicked, this, [this]() {
        if (m_view)
            m_view->zoomIn();
    });

    setZoom(100);
    setEnabled(false);
}

WebView *ZoomControl::webView() const
{
    return m_view;
}

void ZoomControl::setWebView(WebView *view)
{
    if (m_view == view)
        return;
    if (m_view)
        m_view->disconnect(this);
    m_view = view;
    if (!m_view) {
        setZoom(100);
        setEnabled(false);
        return;
    }
    setEnabled(true);
    connect(m_view, &WebView::zoomChanged,
            this, &ZoomControl::setZoom);
    setZoom(m_view->currentZoom());
}

void ZoomControl::setZoom(int zoom)
{
    m_valueButton->setText(tr("%1%").arg(zoom));
}

MemIndicator::MemIndicator(QWidget *parent)
    : QWidget(parent)
    , m_label(new QLabel(this))
    , m_timer(new QTimer(this))
{
    setObjectName(QLatin1String("memIndicator"));
    m_label->setObjectName(QLatin1String("memLabel"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);

    m_label->setAccessibleName(tr("Tab memory usage"));
    m_label->setToolTip(tr("Resident memory of the current tab's "
                           "renderer process, read from "
                           "/proc/<pid>/status every 2 seconds."));
    // Pin the width to the widest expected value so the status bar
    // does not jitter on every poll.
    m_label->setMinimumWidth(
        fontMetrics().horizontalAdvance(QLatin1String("MEM 999.9 MB")));

    m_timer->setInterval(2000);
    connect(m_timer, &QTimer::timeout, this, &MemIndicator::refresh);
    m_timer->start();
    refresh();

#if !defined(Q_OS_LINUX)
    setVisible(false);
    m_timer->stop();
#endif
}

WebView *MemIndicator::webView() const
{
    return m_view;
}

void MemIndicator::setWebView(WebView *view)
{
    if (m_view == view)
        return;
    if (m_view)
        m_view->disconnect(this);
    m_view = view;
    if (m_view && m_view->page()) {
        // Renderer swaps/crashes re-issue the page's pid — refresh
        // immediately instead of waiting out the poll interval.
        connect(m_view->page(), &QWebEnginePage::renderProcessPidChanged,
                this, [this](qint64) { refresh(); });
    }
    refresh();
}

void MemIndicator::refresh()
{
    qint64 pid = -1;
    if (m_view && m_view->page())
        pid = m_view->page()->renderProcessPid();
    const qint64 kb = residentMemoryKb(pid);
    m_label->setText(kb > 0
                     ? tr("MEM %1").arg(formatRss(kb))
                     : tr("MEM —"));
}

qint64 MemIndicator::residentMemoryKb(qint64 pid)
{
#if defined(Q_OS_LINUX)
    if (pid <= 0)
        return -1;
    QFile file(QStringLiteral("/proc/%1/status").arg(pid));
    if (!file.open(QIODevice::ReadOnly))
        return -1;
    const QByteArray status = file.readAll();
    const int line = status.indexOf("VmRSS:");
    if (line == -1)
        return -1;
    const int end = status.indexOf('\n', line);
    const QList<QByteArray> fields = status
        .mid(line, end == -1 ? -1 : end - line)
        .simplified().split(' ');
    // "VmRSS:   238000 kB"
    if (fields.count() < 2)
        return -1;
    bool ok = false;
    const qint64 kb = fields.at(1).toLongLong(&ok);
    return ok ? kb : -1;
#else
    Q_UNUSED(pid);
    return -1;
#endif
}

QString MemIndicator::formatRss(qint64 kb)
{
    const double mb = kb / 1024.0;
    if (mb >= 1024)
        return tr("%1 GB").arg(mb / 1024.0, 0, 'f', 1);
    if (mb >= 100)
        return tr("%1 MB").arg(mb, 0, 'f', 0);
    return tr("%1 MB").arg(mb, 0, 'f', 1);
}

NetIndicator::NetIndicator(QWidget *parent)
    : QWidget(parent)
    , m_label(new QLabel(this))
    , m_timer(new QTimer(this))
    , m_idleTimer(new QTimer(this))
    , m_samplesSinceRescan(0)
    , m_lastReadBytes(0)
    , m_lastWriteBytes(0)
    , m_lastDownloadBytes(0)
    , m_haveIoBaseline(false)
    , m_haveDownloadBaseline(false)
{
    setObjectName(QLatin1String("netIndicator"));
    m_label->setObjectName(QLatin1String("netLabel"));

    QHBoxLayout *layout = new QHBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->addWidget(m_label);

    m_label->setAccessibleName(tr("Network activity"));
    m_label->setToolTip(tr("Approximate engine bandwidth — sampled "
                           "from the network service's /proc io "
                           "counters, which count IPC alongside "
                           "sockets and so run high.  Exact "
                           "download bytes are used while a file "
                           "downloads."));

    m_timer->setInterval(1000);
    connect(m_timer, &QTimer::timeout, this, &NetIndicator::sample);
    m_idleTimer->setSingleShot(true);
    m_idleTimer->setInterval(3000);
    connect(m_idleTimer, &QTimer::timeout,
            this, &NetIndicator::hideWhenIdle);

    setVisible(false);
#if defined(Q_OS_LINUX)
    m_timer->start();
    m_interval.start();
#endif
}

QString NetIndicator::formatRate(qint64 bytesPerSecond)
{
    return DownloadManager::dataString(bytesPerSecond)
        + QLatin1String("/s");
}

void NetIndicator::rescanEnginePids()
{
    m_enginePids.clear();
    m_samplesSinceRescan = 0;
#if defined(Q_OS_LINUX)
    const qint64 appPid = QCoreApplication::applicationPid();
    QList<qint64> enginePids;
    qint64 networkPid = -1;
    const QStringList procs = QDir(QLatin1String("/proc")).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : procs) {
        bool isPid = false;
        const qint64 pid = entry.toLongLong(&isPid);
        if (!isPid)
            continue;
        // Direct child of the browser process?  /proc/<pid>/stat
        // carries ppid right after the last ')' — comm may contain
        // spaces but no closing paren.
        QFile statFile(QLatin1String("/proc/") + entry
                       + QLatin1String("/stat"));
        if (!statFile.open(QIODevice::ReadOnly))
            continue;
        const QByteArray stat = statFile.readAll();
        const int paren = stat.lastIndexOf(')');
        if (paren == -1)
            continue;
        const QList<QByteArray> fields =
            stat.mid(paren + 1).simplified().split(' ');
        if (fields.count() < 2 || fields.at(1).toLongLong() != appPid)
            continue;
        QFile cmdlineFile(QLatin1String("/proc/") + entry
                          + QLatin1String("/cmdline"));
        if (!cmdlineFile.open(QIODevice::ReadOnly))
            continue;
        const QByteArray cmdline = cmdlineFile.readAll();
        if (!cmdline.contains("QtWebEngineProcess"))
            continue;
        enginePids.append(pid);
        // The network service runs as
        // --type=utility --utility-sub-type=network.mojom.NetworkService
        if (cmdline.contains("--type=utility")
            && cmdline.contains("network.mojom.NetworkService"))
            networkPid = pid;
    }
    // The network service alone carries page traffic — prefer it.
    // The fallback sums every engine child, which over-counts with
    // renderer/GPU disk and IPC traffic; the tooltip owns the caveat.
    m_enginePids = (networkPid != -1)
        ? (QList<qint64>() << networkPid) : enginePids;
#endif
}

bool NetIndicator::engineIoTotals(qint64 *readBytes, qint64 *writeBytes)
{
    *readBytes = 0;
    *writeBytes = 0;
#if defined(Q_OS_LINUX)
    // Standalone scan for tests — the widget itself caches the pid
    // list via rescanEnginePids().
    qint64 appPid = QCoreApplication::applicationPid();
    bool found = false;
    const QStringList procs = QDir(QLatin1String("/proc")).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : procs) {
        bool isPid = false;
        const qint64 pid = entry.toLongLong(&isPid);
        if (!isPid)
            continue;
        QFile statFile(QLatin1String("/proc/") + entry
                       + QLatin1String("/stat"));
        if (!statFile.open(QIODevice::ReadOnly))
            continue;
        const QByteArray stat = statFile.readAll();
        const int paren = stat.lastIndexOf(')');
        if (paren == -1)
            continue;
        const QList<QByteArray> fields =
            stat.mid(paren + 1).simplified().split(' ');
        if (fields.count() < 2 || fields.at(1).toLongLong() != appPid)
            continue;
        QFile cmdlineFile(QLatin1String("/proc/") + entry
                          + QLatin1String("/cmdline"));
        if (!cmdlineFile.open(QIODevice::ReadOnly))
            continue;
        if (!cmdlineFile.readAll().contains("QtWebEngineProcess"))
            continue;
        QFile ioFile(QLatin1String("/proc/") + entry
                     + QLatin1String("/io"));
        if (!ioFile.open(QIODevice::ReadOnly))
            continue;
        const QByteArray io = ioFile.readAll();
        for (const QByteArray &line : io.split('\n')) {
            const QList<QByteArray> kv = line.simplified().split(' ');
            if (kv.count() != 2)
                continue;
            if (kv.at(0) == "rchar:")
                *readBytes += kv.at(1).toLongLong();
            else if (kv.at(0) == "wchar:")
                *writeBytes += kv.at(1).toLongLong();
        }
        found = true;
    }
    return found;
#else
    Q_UNUSED(readBytes);
    Q_UNUSED(writeBytes);
    return false;
#endif
}

void NetIndicator::sample()
{
#if defined(Q_OS_LINUX)
    // Refresh the pid set periodically so newly spawned renderers and
    // a replaced network service are picked up, and whenever every
    // cached pid has died.
    if (m_enginePids.isEmpty() || ++m_samplesSinceRescan >= 5)
        rescanEnginePids();

    qint64 readBytes = 0, writeBytes = 0;
    int readable = 0;
    for (const qint64 pid : m_enginePids) {
        QFile ioFile(QStringLiteral("/proc/%1/io").arg(pid));
        if (!ioFile.open(QIODevice::ReadOnly))
            continue;
        ++readable;
        const QByteArray io = ioFile.readAll();
        for (const QByteArray &line : io.split('\n')) {
            const QList<QByteArray> kv = line.simplified().split(' ');
            if (kv.count() != 2)
                continue;
            if (kv.at(0) == "rchar:")
                readBytes += kv.at(1).toLongLong();
            else if (kv.at(0) == "wchar:")
                writeBytes += kv.at(1).toLongLong();
        }
    }
    if (readable == 0 && !m_enginePids.isEmpty()) {
        // Every cached pid is gone — force a rescan next sample.
        m_enginePids.clear();
    }

    // Scale the deltas by the real elapsed interval — timer slack
    // makes it a little longer than 1s.
    const double secs = m_interval.isValid()
        ? m_interval.restart() / 1000.0 : 1.0;
    qint64 downRate = 0, upRate = 0;
    if (m_haveIoBaseline && secs > 0) {
        downRate = qint64(qMax<qint64>(0, readBytes - m_lastReadBytes)
                          / secs);
        upRate = qint64(qMax<qint64>(0, writeBytes - m_lastWriteBytes)
                        / secs);
    }
    m_lastReadBytes = readBytes;
    m_lastWriteBytes = writeBytes;
    m_haveIoBaseline = true;

    // Exact transfer accounting while downloads run — the manager's
    // receivedBytes deltas count payload only and replace the io
    // approximation on the down side.
    DownloadManager *dm = DownloadManager::instance();
    qint64 downloadBytes = 0;
    bool haveDownloads = false;
    if (dm) {
        DownloadModel *model = dm->model();
        const int rows = model ? model->rowCount() : 0;
        for (int i = 0; i < rows; ++i) {
            DownloadItem *item = dm->itemAt(i);
            if (item && item->downloading()) {
                downloadBytes += item->bytesReceived();
                haveDownloads = true;
            }
        }
    }
    if (haveDownloads) {
        if (m_haveDownloadBaseline && secs > 0)
            downRate = qint64(
                qMax<qint64>(0, downloadBytes - m_lastDownloadBytes)
                / secs);
        m_haveDownloadBaseline = true;
    } else {
        m_haveDownloadBaseline = false;
    }
    m_lastDownloadBytes = downloadBytes;

    if (downRate <= 0 && upRate <= 0)
        return;  // idle — the grace timer hides us after ~3s.

    m_label->setText(tr("↓ %1 ↑ %2")
                     .arg(formatRate(downRate), formatRate(upRate)));
    setVisible(true);
    m_idleTimer->start();
#endif
}

void NetIndicator::hideWhenIdle()
{
    setVisible(false);
}
