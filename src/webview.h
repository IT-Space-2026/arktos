/*
 * Copyright 2008-2009 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2008 Ariya Hidayat <ariya.hidayat@gmail.com>
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#ifndef WEBVIEW_H
#define WEBVIEW_H

#include <qwebengineview.h>

#include "tabwidget.h"

class QWebEngineProfile;
class BrowserMainWindow;
class PictureInPicture;
class ReaderMode;
class ScriptBlockInfoBar;
class TabWidget;
class WebPage;
class WebView : public QWebEngineView
{
    Q_OBJECT

public:
    WebView(QWidget *parent = nullptr);
    // Creates the view's WebPage on the given profile (used to point tabs
    // at the private off-the-record profile, for example).
    WebView(QWebEngineProfile *profile, QWidget *parent = nullptr);
    WebPage *webPage() const { return m_page; }

    void loadSettings();

    void loadUrl(const QUrl &url, const QString &title = QString());
    // Whether a url that arrived from outside the browser chrome — the
    // command line, a forwarded second-instance message, a drop, a
    // pasted selection — may be handed to loadUrl().  javascript: is
    // refused on those paths: it would run script in the current
    // page's origin.  Typed input and bookmarklets reach loadUrl()
    // through the trusted path (SEC02/SEC09).
    static bool isUrlAllowedOnUntrustedInput(const QUrl &url);
    QUrl url() const;

    QString lastStatusBarText() const;
    inline int progress() const { return m_progress; }
    inline int currentZoom() const { return m_currentZoom; }
    TabWidget *tabWidget() const;
    // CONT02: the container this view's page is bound to — the empty
    // (default) container id for the normal and off-the-record
    // profiles, the container's id for container profiles.
    QString containerId() const;
    // JSCTL: whether the current page's scripts are blocked.
    bool isJavaScriptBlocked() const;
    // READ01: reader-mode controller for this view (never null).
    ReaderMode *readerMode() const { return m_readerMode; }
    // PIP01: Picture-in-Picture controller for this view (never
    // null).  QtWebEngine has no PiP delegate — the video pops out
    // into an app-side floating window.
    PictureInPicture *pictureInPicture() const { return m_pip; }

signals:
    void search(const QUrl &searchUrl, TabWidget::OpenUrlIn openIn);
    void statusBarMessage(const QString &string);
    // UIP04: every zoom path (menu/keyboard/Ctrl+wheel/status-bar
    // control) funnels through applyZoom(), which re-emits this so
    // chrome widgets can track the current percent.
    void zoomChanged(int zoom);
    // JSCTL: forwarded from WebPage — the shield badge listens here.
    void javaScriptBlockedChanged(bool blocked);

public slots:
    void zoomIn();
    void zoomOut();
    void resetZoom();
    void applyZoom();
    // READ01: enter/exit reader mode on this view.
    void toggleReaderMode();

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private:
    int levelForZoom(int zoom);
    void init();
    void openUrlInTarget(const QUrl &linkUrl, TabWidget::OpenUrlIn target);
    void updateScriptBlockBar(bool blocked);
    void allowScriptsOnThisSite(bool persistent);

private slots:
    void setProgress(int progress);
    void loadFinished();
    void setStatusBarText(QString string);
    void openActionUrlInNewTab();
    void openActionUrlInNewWindow();
    void downloadLinkToDisk();
    void copyLinkToClipboard();
    void downloadImageToDisk();
    void copyImageToClipboard();
    void copyImageLocationToClipboard();
    void blockImage();
    void bookmarkLink();
    void searchRequested(QAction *action);
    void imageSearchRequested();

private:
    QString m_statusBarText;
    QUrl m_initialUrl;
    int m_progress;
    int m_currentZoom;
    QList<int> m_zoomLevels;
    WebPage *m_page;
    ScriptBlockInfoBar *m_scriptBlockBar;
    ReaderMode *m_readerMode;
    PictureInPicture *m_pip;
};

#endif

