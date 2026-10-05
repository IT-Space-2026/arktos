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

#include "autofillmanager.h"

#include "autosaver.h"
#include "browserpaths.h"

#include <qfile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmessagebox.h>
#include <qpointer.h>
#include <qset.h>
#include <qsettings.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

// #define AUTOFILL_DEBUG

// Qt WebEngine has neither the POST-body interception the Qt4 version
// used to observe form submits (QtNetwork no longer sees web traffic)
// nor synchronous DOM access.  Both sides now live in the render
// process: autofill.js is injected after each successful load, fills
// stored values, and reports submits through the "aroraAutofill"
// QWebChannel object.  Known deltas vs the WebKit implementation:
// forms submitted before the channel handshake completes (a few ms
// after load) can be missed, submits inside iframes are not captured
// (the old code only looked at the main frame too), and a submit that
// navigates immediately may race the channel message delivery.

AutoFillBridge::AutoFillBridge(QObject *parent)
    : QObject(parent)
    , m_captureEnabled(false)
{
}

void AutoFillBridge::setPageInfo(const QUrl &pageUrl, bool captureEnabled)
{
    m_pageUrl = pageUrl;
    m_captureEnabled = captureEnabled;
}

void AutoFillBridge::submitForm(const QString &reportedUrl,
        const QVariantMap &formData)
{
    if (!m_captureEnabled)
        return;
    AutoFillManager::instance()->formSubmitted(m_pageUrl, reportedUrl, formData);
}

AutoFillManager *AutoFillManager::instance()
{
    static QPointer<AutoFillManager> manager;
    if (!manager)
        manager = new AutoFillManager(qApp);
    return manager;
}

AutoFillManager::AutoFillManager(QObject *parent)
    : QObject(parent)
    , m_savePasswordForms(true)
    , m_allowAutoCompleteOff(true)
    , m_saveTimer(new AutoSaver(this))
{
    connect(this, &AutoFillManager::autoFillChanged,
            m_saveTimer, &AutoSaver::changeOccurred);
    loadSettings();
    loadFormData();
}

AutoFillManager::~AutoFillManager()
{
    m_saveTimer->saveIfNeccessary();
}

void AutoFillManager::save() const
{
    saveFormData();
}

void AutoFillManager::setForms(const QList<Form> &forms)
{
    m_forms = forms;
    emit autoFillChanged();
}

QList<AutoFillManager::Form> AutoFillManager::forms() const
{
    return m_forms;
}

void AutoFillManager::loadSettings()
{
    QSettings settings;
    settings.beginGroup(QLatin1String("autofill"));
    m_savePasswordForms = settings.value(QLatin1String("passwordForms"), m_savePasswordForms).toBool();
    m_allowAutoCompleteOff = settings.value(QLatin1String("ignoreAutoCompleteOff"), m_allowAutoCompleteOff).toBool();
}

QString AutoFillManager::autoFillDataFile()
{
    return BrowserPaths::dataFilePath(QLatin1String("autofill.dat"));
}

void AutoFillManager::saveFormData() const
{
    QString fileName = autoFillDataFile();
    QFile file(fileName);
    if (!file.open(QFile::WriteOnly)) {
        qWarning() << "Unable to open" << fileName << "to store autofill data";
        return;
    }

    QDataStream stream(&file);
    stream << m_forms;
}

void AutoFillManager::loadFormData()
{
    QString fileName = autoFillDataFile();
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly))
        return;

    QDataStream stream(&file);
    stream >> m_forms;
}

void AutoFillManager::attachToPage(QWebEnginePage *page)
{
    if (!page)
        return;

    // Private browsing is per-profile now: pages on the off-the-record
    // profile get the fill pass (parity with the old global private
    // mode) but no submit capture, and their bridge drops reports.
    AutoFillBridge *bridge = page->findChild<AutoFillBridge *>();
    const bool capture = !page->profile()->isOffTheRecord();
    if (bridge)
        bridge->setPageInfo(page->url(), capture);

    const QList<Form> forms = fetchForms(stripUrl(page->url()));
    page->runJavaScript(autoFillScript(forms, capture && bridge));
}

QString AutoFillManager::autoFillScript(const QList<Form> &forms, bool capture) const
{
    static const QString script = [] {
        QString source;
        // Embedding the channel client keeps the injected script
        // self-contained; the JS still falls back to a qrc:// script
        // tag if the resource were unreachable.
        QFile channelFile(QLatin1String(":/qtwebchannel/qwebchannel.js"));
        if (channelFile.open(QIODevice::ReadOnly))
            source += QString::fromUtf8(channelFile.readAll());
        QFile file(QLatin1String(":autofill.js"));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning() << "AutoFillManager:" << "Unable to open js autofill file";
            return source;
        }
        source += QString::fromUtf8(file.readAll());
        return source;
    }();

    QJsonArray formsJson;
    for (const Form &form : forms) {
        QJsonObject formJson;
        formJson[QLatin1String("name")] = form.name;
        QJsonArray elementsJson;
        for (const Element &element : form.elements) {
            QJsonObject elementJson;
            elementJson[QLatin1String("name")] = element.first;
            elementJson[QLatin1String("value")] = element.second;
            elementsJson.append(elementJson);
        }
        formJson[QLatin1String("elements")] = elementsJson;
        formsJson.append(formJson);
    }

    QString injected = script;
    injected.replace(QLatin1String("FORMS_JSON"),
                     QString::fromUtf8(QJsonDocument(formsJson).toJson(QJsonDocument::Compact)));
    injected.replace(QLatin1String("CAPTURE_FLAG"),
                     capture ? QLatin1String("true") : QLatin1String("false"));
    return injected;
}

void AutoFillManager::formSubmitted(const QUrl &pageUrl,
        const QString &reportedUrl, const QVariantMap &formData)
{
#ifdef AUTOFILL_DEBUG
    qDebug() << "AutoFillManager::" << __FUNCTION__ << pageUrl << formData;
#endif

    const QUrl url = stripUrl(pageUrl);
    // The bridge's tagged url is trusted; the report's own claim must
    // match it or the data is forged/stale and dropped.
    if (stripUrl(QUrl::fromEncoded(reportedUrl.toUtf8())) != url)
        return;

    // Check that the url isn't in m_never
    if (m_never.contains(url))
        return;

    Form form;
    form.url = url;
    form.name = formData[QLatin1String("name")].toString();
    form.hasAPassword = formData[QLatin1String("hasPassword")].toBool();

    QSet<Element> liveElements;
    QSet<Element> deadElements;
    const QVariantList elements = formData[QLatin1String("elements")].toList();
    for (const QVariant &element : elements) {
        const QVariantMap map = element.toMap();
        const QString name = map[QLatin1String("name")].toString();
        if (name.isEmpty())
            continue;
        const Element pair(name, map[QLatin1String("value")].toString());
        liveElements.insert(pair);
        if (map[QLatin1String("autocomplete")].toString() == QLatin1String("off"))
            deadElements.insert(pair);
    }
    // Same quirk as the Qt4 code: when ignoreAutoCompleteOff is set the
    // autocomplete="off" fields are dropped from the stored set.
    if (m_allowAutoCompleteOff)
        liveElements.subtract(deadElements);
    form.elements = liveElements.values();
    if (!form.isValid())
        return;

    // Check if we allow storing this form if it has a password
    if (!allowedToAutoFill(form.hasAPassword))
        return;

    // Prompt if we have never seen this password
    int alreadyAccepted = -1;
    for (int i = 0; i < m_forms.count(); ++i) {
        if (m_forms.at(i).url == url) {
            alreadyAccepted = i;
            break;
        }
    }
    if (form.hasAPassword && alreadyAccepted == -1 && !promptToSave(url))
        return;

#ifdef AUTOFILL_DEBUG
    qDebug() << "AutoFillManager:" << "Saving" << form.url;
#endif
    // TODO When we can hook into element events we can save multiple passwords for different users
    if (alreadyAccepted != -1)
        m_forms.removeAt(alreadyAccepted);
    m_forms.append(form);
    emit autoFillChanged();
}

bool AutoFillManager::promptToSave(const QUrl &url)
{
    QMessageBox messageBox;
    messageBox.setText(tr("<b>Would you like to save this password?</b><br> \
    To review passwords you have saved and remove them, open the AutoFill panel of preferences."));
    messageBox.addButton(tr("Never for this site"), QMessageBox::DestructiveRole);
    messageBox.addButton(tr("Not now"), QMessageBox::RejectRole);
    messageBox.addButton(QMessageBox::Yes);
    messageBox.setDefaultButton(QMessageBox::Yes);
    messageBox.exec();
    switch (messageBox.buttonRole(messageBox.clickedButton())) {
    case QMessageBox::DestructiveRole:
        m_never.append(url);
        return false;
    case QMessageBox::RejectRole:
        return false;
    default:
        return true;
    }
}

QUrl AutoFillManager::stripUrl(const QUrl &url)
{
    QUrl cleanUrl = url;
    cleanUrl.setQuery(QString());
    cleanUrl.setFragment(QString());
    cleanUrl.setUserInfo(QString());
    return cleanUrl;
}

bool AutoFillManager::allowedToAutoFill(bool password) const
{
    if (password && m_savePasswordForms)
        return true;
    return false;
}

QList<AutoFillManager::Form> AutoFillManager::fetchForms(const QUrl &url) const
{
    QList<Form> forms;
    for (const Form &form : m_forms)
        if (form.url == url)
            forms.append(form);
#ifdef AUTOFILL_DEBUG
    qDebug() << "AutoFillManager::" << __FUNCTION__ << url << m_forms.count() << "found:" << forms.count();
#endif
    return forms;
}

QDataStream &operator>>(QDataStream &in, AutoFillManager::Form &form)
{
    AutoFillManager::Form::load(in, form);
    return in;
}

QDataStream &operator<<(QDataStream &out, const AutoFillManager::Form &form)
{
    AutoFillManager::Form::save(out, form);
    return out;
}

void AutoFillManager::Form::load(QDataStream &in, AutoFillManager::Form &form)
{
    in >> form.elements;
    in >> form.url;
    in >> form.name;
    in >> form.hasAPassword;
}

void AutoFillManager::Form::save(QDataStream &out, const AutoFillManager::Form &form)
{
    out << form.elements;
    out << form.url;
    out << form.name;
    out << form.hasAPassword;
}
