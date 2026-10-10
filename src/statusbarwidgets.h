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
#ifndef STATUSBARWIDGETS_H
#define STATUSBARWIDGETS_H

#include <qelapsedtimer.h>
#include <qpointer.h>
#include <qwidget.h>

class QLabel;
class QProgressBar;
class QTimer;
class QToolButton;
class WebView;

// UIP04: permanent right-side status-bar widgets (Vivaldi-style).
// Both bind to the current tab's WebView via setWebView() and are
// re-pointed by BrowserMainWindow on every tab switch; both tolerate
// a null view (window without tabs).

// Compact load indicator: slim progress bar + a running elapsed time
// while a page loads; on completion it shows "Loaded in N s" for a
// few seconds, then clears itself.  Hidden while idle.  It lives
// outside the temporary-message area so hover-link text does not
// clobber it.
class LoadingIndicator : public QWidget
{
    Q_OBJECT

public:
    explicit LoadingIndicator(QWidget *parent = nullptr);

    void setWebView(WebView *view);
    WebView *webView() const;
    bool loading() const { return m_loading; }
    // Elapsed time of the last finished load (test/diagnostic hook).
    qint64 lastElapsedMs() const { return m_lastElapsedMs; }

private slots:
    void pageLoadStarted();
    void pageLoadProgress(int progress);
    void pageLoadFinished(bool ok);
    void updateElapsed();
    void clearStatus();

private:
    QPointer<WebView> m_view;
    QProgressBar *m_bar;
    QLabel *m_label;
    QElapsedTimer m_elapsed;
    QTimer *m_tick;
    QTimer *m_clearTimer;
    bool m_loading;
    qint64 m_lastElapsedMs;
};

// Zoom control: "- [100%] +" — clicking the percent resets to 100%.
// Follows the bound view's zoomChanged() signal so menu, keyboard and
// Ctrl+wheel zoom update the display too.
class ZoomControl : public QWidget
{
    Q_OBJECT

public:
    explicit ZoomControl(QWidget *parent = nullptr);

    void setWebView(WebView *view);
    WebView *webView() const;

private slots:
    void setZoom(int zoom);

private:
    QPointer<WebView> m_view;
    QToolButton *m_outButton;
    QToolButton *m_valueButton;
    QToolButton *m_inButton;
};

// SBAR01: current tab's renderer memory — reads VmRSS out of
// /proc/<renderProcessPid>/status every ~2s and shows "MEM 238 MB".
// Follows tab switches through setWebView() like the other widgets
// and re-resolves the pid on every poll (plus on
// renderProcessPidChanged), so renderer swaps and crashes are picked
// up without extra bookkeeping; an unreadable pid shows "MEM —".
// Linux-only: the widget stays hidden on platforms without /proc.
class MemIndicator : public QWidget
{
    Q_OBJECT

public:
    explicit MemIndicator(QWidget *parent = nullptr);

    void setWebView(WebView *view);
    WebView *webView() const;

    // VmRSS in kB read from /proc/<pid>/status, -1 when unreadable
    // (dead process, bogus pid, non-Linux).  Static so tests can
    // drive it without a WebEngine renderer.
    static qint64 residentMemoryKb(qint64 pid);
    // kB -> display string: "87.4 MB" under 100MB, "238 MB" from
    // there, "1.5 GB" past a gigabyte.
    static QString formatRss(qint64 kb);

private slots:
    void refresh();

private:
    QPointer<WebView> m_view;
    QLabel *m_label;
    QTimer *m_timer;
};

// SBAR01: live bandwidth — samples /proc/<pid>/io on the app's
// QtWebEngineProcess children every ~1s and shows
// "↓ 1.2 MB/s ↑ 84 kB/s" while traffic is moving.  The preferred
// source is the network-service utility process (its cmdline carries
// --type=utility plus the network.mojom.NetworkService sub-type);
// when it cannot be identified every engine child is summed instead.
//
// Honest caveat: /proc io counters are syscall totals, not wire
// bytes — rchar/wchar cover sockets AND the mojo IPC the same process
// does, so the figure over-counts (read_bytes/write_bytes only track
// storage and were unusable here).  While a download is in flight the
// exact DownloadManager receivedBytes deltas replace the down-rate,
// which removes the approximation where it matters most.  The
// tooltip says all of this.  Hidden after ~3s without traffic, and
// always hidden off Linux.
class NetIndicator : public QWidget
{
    Q_OBJECT

public:
    explicit NetIndicator(QWidget *parent = nullptr);

    // bytes/second -> "84.0 kB/s" style string (shares the
    // DownloadManager unit ladder).
    static QString formatRate(qint64 bytesPerSecond);
    // Sums the rchar/wchar io counters of the in-scope engine
    // processes into the out-params.  False when no engine child
    // could be read (non-Linux, engine not started yet).
    static bool engineIoTotals(qint64 *readBytes, qint64 *writeBytes);

private slots:
    void sample();
    void hideWhenIdle();

private:
    void rescanEnginePids();

    QLabel *m_label;
    QTimer *m_timer;
    QTimer *m_idleTimer;
    QElapsedTimer m_interval;
    QList<qint64> m_enginePids;
    int m_samplesSinceRescan;
    qint64 m_lastReadBytes;
    qint64 m_lastWriteBytes;
    qint64 m_lastDownloadBytes;
    bool m_haveIoBaseline;
    bool m_haveDownloadBaseline;
};

#endif // STATUSBARWIDGETS_H
