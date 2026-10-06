/*
 * Copyright 2009 Benjamin C. Meyer <ben@meyerhome.net>
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

#ifndef WEBPAGE_H
#define WEBPAGE_H

#include "tabwidget.h"

#include <qlist.h>
#include <qwebenginecertificateerror.h>
#include <qwebenginepage.h>

#include <functional>

class WebPageLinkedResource
{
public:
    QString rel;
    QString type;
    QUrl href;
    QString title;
};

class QWebEngineLoadingInfo;
class QWebChannel;
class AutoFillBridge;
class OpenSearchEngine;
// See https://developer.mozilla.org/en/adding_search_engines_from_web_pages
class JavaScriptExternalObject : public QObject
{
    Q_OBJECT

public:
    JavaScriptExternalObject(QObject *parent = 0);

public slots:
    void AddSearchProvider(const QString &url);
};

class JavaScriptAroraObject : public QObject
{
    Q_OBJECT

    // SEC08: a plain string, never a QObject* — a live engine object
    // handed to JS would expose every writable property and public
    // slot of OpenSearchEngine to the page.
    Q_PROPERTY(QString currentEngineName READ currentEngineName
               NOTIFY currentEngineNameChanged)

public:
    JavaScriptAroraObject(QObject *parent = 0);

    QString currentEngineName() const;

signals:
    void currentEngineNameChanged();

public slots:
    QString translate(const QString &string);
    QString searchUrl(const QString &string) const;
};

class WebPage : public QWebEnginePage
{
    Q_OBJECT

signals:
    void aboutToLoadUrl(const QUrl &url);
    // SEC06: emitted when a certificate error is deferred and the
    // interstitial page has been shown; the decision is resolved via
    // the interstitial's action links.
    void certificateErrorInterstitial(const QUrl &url);

public:
    WebPage(QObject *parent = 0);
    WebPage(QWebEngineProfile *profile, QObject *parent = 0);

    void loadSettings();

    // Qt WebEngine has no synchronous DOM access; the linked resources are
    // collected in the render process and reported through the callback.
    void linkedResources(const QString &relation,
                         const std::function<void(const QList<WebPageLinkedResource> &)> &resultCallback);
    void linkedResources(const std::function<void(const QList<WebPageLinkedResource> &)> &resultCallback);

    static QString userAgent();
    static void setUserAgent(const QString &userAgent);

protected:
    bool acceptNavigationRequest(const QUrl &url, NavigationType type, bool isMainFrame);
    QWebEnginePage *createWindow(QWebEnginePage::WebWindowType type);

private slots:
    void handleLoadingChanged(const QWebEngineLoadingInfo &loadingInfo);

private:
    void init();
    void showErrorPage(const QUrl &url, const QString &errorString);
    void confirmAndOpenExternalUrl(const QUrl &url);
    void handleCertificateError(QWebEngineCertificateError error);
    QString certificateErrorHtml(const QWebEngineCertificateError &error);
    void resolveCertificateErrorLink(const QUrl &url);

protected:
    static QString s_userAgent;
    TabWidget::OpenUrlIn m_openTargetBlankLinksIn;
    QUrl m_requestedUrl;
    JavaScriptExternalObject *m_javaScriptExternalObject;
    JavaScriptAroraObject *m_javaScriptAroraObject;
    AutoFillBridge *m_autoFillBridge;
    QWebChannel *m_webChannel;
    QWebEngineCertificateError m_pendingCertError;
    QString m_certErrorNonce;
    bool m_certErrorPending;
};

#endif // WEBPAGE_H
