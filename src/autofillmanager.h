/**
 * Copyright (c) 2009, Benjamin C. Meyer  <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef AUTOFILLMANAGER_H
#define AUTOFILLMANAGER_H

#include <qobject.h>

#include <qurl.h>
#include <qvariantmap.h>

class QDataStream;
class QWebEnginePage;
class AutoSaver;
namespace Engine { class Page; }

// Per-page bridge exposed to JavaScript as "aroraAutofill" through the
// page's QWebChannel (registered in WebPage::init).  The injected
// autofill.js reports submitted forms through submitForm(); the bridge
// tags each report with the url the page had when the hook was
// installed so a page cannot forge form data for a different origin.
// SEC08: the channel object is reachable from any page script that
// loads qwebchannel.js, so reports additionally carry a per-load token
// that lives only inside the injected script's closure — calls without
// it (the page's own channel client has no way to learn it) are dropped.
class AutoFillBridge : public QObject
{
    Q_OBJECT

public:
    AutoFillBridge(QObject *parent = nullptr);

    void setPageInfo(const QUrl &pageUrl, bool captureEnabled,
                     const QString &reportToken);

public slots:
    void submitForm(const QString &reportToken, const QString &reportedUrl,
                    const QVariantMap &formData);

private:
    QUrl m_pageUrl;
    QString m_reportToken;
    bool m_captureEnabled;
};

class AutoFillManager : public QObject
{
    Q_OBJECT

signals:
    void autoFillChanged();

public:
    typedef QPair<QString, QString> Element;
    class Form {
    public:
        bool isValid() const { return !elements.isEmpty(); }
        static void load(QDataStream &in, Form &form);
        static void save(QDataStream &out, const Form &form);

        QList<Element> elements;
        QUrl url;
        QString name;
        bool hasAPassword;
    };

    // Lazy qApp-owned singleton (same pattern as HistoryManager /
    // BookmarksManager / AdBlockManager).  BrowserApplication delegates
    // here; the manager reads the profile-independent store itself.
    static AutoFillManager *instance();

    AutoFillManager(QObject *parent = nullptr);
    ~AutoFillManager();

    void loadSettings();

    // Immediate path: inject the bundle into the already-loaded
    // document through runJavaScript (tests + the audit-bisection
    // harness).  ENG04: takes the engine-neutral page — the engine
    // page is resolved inside.
    void attachToPage(Engine::Page *page);

    // SEC16: arms the same bundle as a named per-page QWebEngineScript
    // at DocumentReady keyed to the navigation's target url — the
    // injection rides Chromium's own script pipeline instead of
    // competing with paint-gated subresource scheduling after load.
    void scheduleOnPage(QWebEnginePage *page, const QUrl &url);

    void setForms(const QList<Form> &forms);
    QList<Form> forms() const;

public slots:
    // Invoked by the page's AutoFillBridge when the injected script
    // reports a submitted form.
    void formSubmitted(const QUrl &pageUrl, const QString &reportedUrl,
                       const QVariantMap &formData);

private slots:
    void save() const;

private:
    static QUrl stripUrl(const QUrl &url);
    static QString autoFillDataFile();
    bool allowedToAutoFill(bool password) const;
    QList<AutoFillManager::Form> fetchForms(const QUrl &url) const;
    QString scriptForPage(QWebEnginePage *page, const QUrl &url);
    bool captureEnabledForPage(QWebEnginePage *page) const;
    QString autoFillScript(const QList<Form> &forms, bool capture,
                           const QString &reportToken) const;
    bool promptToSave(const QUrl &url);

    void saveFormData() const;
    void loadFormData();

    bool m_savePasswordForms;
    bool m_allowAutoCompleteOff;

    QList<Form> m_forms;
    QList<QUrl> m_never;
    AutoSaver *m_saveTimer;
};

QDataStream &operator<<(QDataStream &, const AutoFillManager::Form &form);
QDataStream &operator>>(QDataStream &, AutoFillManager::Form &form);

#endif // AUTOFILLMANAGER_H
