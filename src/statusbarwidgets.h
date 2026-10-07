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

#endif // STATUSBARWIDGETS_H
