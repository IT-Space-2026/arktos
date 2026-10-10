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
#include "engineinterface.h"
#include "securestore.h"
#include "startupprofile.h"
#include "streamingutils.h"
#include "webenginebackend.h"

#ifdef ARORA_RUSTCORE
#include "rustcorebridge.h"

#include <rustcore.h>
#endif

#include <qdatastream.h>
#include <qfile.h>
#include <qsavefile.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qmessagebox.h>
#include <qpointer.h>
#include <qset.h>
#include <qsettings.h>
#include <quuid.h>
#include <qwebenginepage.h>
#include <qwebengineprofile.h>

#include <qdebug.h>

// #define AUTOFILL_DEBUG

// Qt WebEngine has neither the POST-body interception the Qt4 version
// used to observe form submits (QtNetwork no longer sees web traffic)
// nor synchronous DOM access.  Both sides now live in the render
// process: autofill.js is armed per navigation as a page user script
// that runs at DocumentReady, fills stored values, and reports submits
// through the "aroraAutofill" QWebChannel object.  Known deltas vs the
// WebKit implementation: forms submitted before the channel handshake
// completes (a few ms after DOMContentLoaded) can be missed, submits
// inside iframes are not captured (the old code only looked at the
// main frame too), and a submit that navigates immediately may race
// the channel message delivery.

AutoFillBridge::AutoFillBridge(QObject *parent)
    : QObject(parent)
    , m_captureEnabled(false)
{
}

void AutoFillBridge::setPageInfo(const QUrl &pageUrl, bool captureEnabled,
        const QString &reportToken)
{
    m_pageUrl = pageUrl;
    m_captureEnabled = captureEnabled;
    m_reportToken = reportToken;
}

void AutoFillBridge::submitForm(const QString &reportToken,
        const QString &reportedUrl, const QVariantMap &formData)
{
    // A token the injected script does not carry is proof the call did
    // not come from it — the object is reachable from arbitrary page
    // script via the shared channel transport.
    if (!m_captureEnabled || m_reportToken.isEmpty()
        || reportToken != m_reportToken)
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
    StartupProfile::Scope profileScope("autofill load");
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

#ifdef ARORA_RUSTCORE
// RCORE05: the canonical store is rustcore's autofill-store.dat — a
// sealed ARSEC1 blob under the same custody key as credentials.dat,
// so one unlock opens both and every passphrase/lock transition
// covers it.  The legacy autofill.dat is parsed once below by the
// original bounded reader, replayed through rc_autofill_set_forms,
// and retired — it is never re-read afterwards, which keeps a
// no-Rust build free of a payload it cannot parse either (the new
// file has its own name).

static QByteArray formsToJson(const QList<AutoFillManager::Form> &forms)
{
    QJsonArray all;
    for (const AutoFillManager::Form &form : forms) {
        QJsonObject formJson;
        formJson[QLatin1String("url")] =
            QString::fromUtf8(form.url.toEncoded());
        formJson[QLatin1String("name")] = form.name;
        formJson[QLatin1String("has_password")] = form.hasAPassword;
        QJsonArray elementsJson;
        for (const AutoFillManager::Element &element : form.elements)
            elementsJson.append(
                QJsonArray{element.first, element.second});
        formJson[QLatin1String("elements")] = elementsJson;
        all.append(formJson);
    }
    return QJsonDocument(all).toJson(QJsonDocument::Compact);
}

static QList<AutoFillManager::Form> formsFromJson(const QByteArray &json)
{
    QList<AutoFillManager::Form> forms;
    const QJsonArray all = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : all) {
        const QJsonObject formJson = value.toObject();
        AutoFillManager::Form form;
        form.url = QUrl::fromEncoded(
            formJson.value(QLatin1String("url")).toString().toUtf8());
        form.name = formJson.value(QLatin1String("name")).toString();
        form.hasAPassword =
            formJson.value(QLatin1String("has_password")).toBool();
        const QJsonArray elementsJson =
            formJson.value(QLatin1String("elements")).toArray();
        for (const QJsonValue &entry : elementsJson) {
            const QJsonArray pair = entry.toArray();
            if (pair.size() == 2) {
                form.elements.append(qMakePair(
                    pair.at(0).toString(), pair.at(1).toString()));
            }
        }
        if (form.isValid())
            forms.append(form);
    }
    return forms;
}

// Decodes raw autofill.dat contents — a sealed ARSEC1 blob or the
// Qt4-era plaintext QDataStream — through the original bounded
// reader.  Returns false on an unopenable/corrupt blob.
static bool decodeLegacyForms(const QByteArray &raw,
                              QList<AutoFillManager::Form> *out)
{
    QByteArray payload = raw;
    if (SecureStore::isSealed(raw)) {
        bool ok = false;
        payload = SecureStore::open(raw, &ok);
        if (!ok)
            return false;
    }
    QList<AutoFillManager::Form> forms;
    QDataStream stream(payload);
    StreamingUtils::readBoundedList(stream, forms);
    if (stream.status() != QDataStream::Ok)
        return false;
    *out = forms;
    return true;
}

// The import consumed the legacy file: move it out of the live path
// so neither store re-reads it.  The renamed copy (still sealed
// ciphertext) is kept as a recoverable backup.
static void retireLegacyFile(const QString &fileName)
{
    if (!QFile::exists(fileName))
        return;
    const QString retired = fileName + QLatin1String(".migrated");
    QFile::remove(retired);
    if (!QFile::rename(fileName, retired))
        qWarning() << "AutoFillManager: cannot retire" << fileName;
}
#endif // ARORA_RUSTCORE

void AutoFillManager::saveFormData() const
{
    QString fileName = autoFillDataFile();

#ifdef ARORA_RUSTCORE
    // Persist into the Rust store.  There is deliberately no
    // plaintext fallback here: the old degrade wrote the legacy file
    // only when no crypto backend existed at all, while a failed
    // write now means the user declined the unlock or the disk
    // refused — serializing PII outside custody in either case would
    // be a leak, so the data stays in memory for this session.
    rustCoreEnsureDataDir();
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        SecureStore::ensureUnlocked(nullptr);

    const QByteArray json = formsToJson(m_forms);
    const RcStatus status = rc_autofill_set_forms(
        reinterpret_cast<const uint8_t *>(json.constData()),
        size_t(json.size()));
    if (status == RC_OK) {
        // Retire a straggler legacy file too (e.g. one written while
        // the store was locked and never re-imported).
        retireLegacyFile(fileName);
        return;
    }
    char *error = rc_last_error_message();
    qWarning() << "AutoFillManager: the autofill store refused the"
                  " write — keeping the data in memory only:"
               << QString::fromUtf8(error ? error : "");
    rc_string_free(error);
    return;
#else
    // Stored forms can carry passwords: the file is sealed with
    // AES-256-GCM (SecureStore) instead of the Qt4-era plaintext
    // QDataStream.  The blob keeps the same stream payload inside, so
    // Form::save/load is unchanged.
    QByteArray payload;
    {
        QDataStream stream(&payload, QIODevice::WriteOnly);
        stream << m_forms;
    }

    const QByteArray sealed = SecureStore::seal(payload);
    if (sealed.isEmpty()) {
        // No crypto backend on this box: never re-serialize password
        // forms as plaintext — keep them in memory for this session
        // and leave whatever file was there untouched.  Password-free
        // forms hold no credentials and still get the legacy store.
        bool hasPassword = false;
        for (const Form &form : m_forms)
            hasPassword |= form.hasAPassword;
        if (hasPassword) {
            qWarning() << "AutoFillManager: secure store unavailable;"
                       << "not persisting autofill data to" << fileName;
            return;
        }
        QFile file(fileName);
        if (!file.open(QFile::WriteOnly)) {
            qWarning() << "Unable to open" << fileName
                       << "to store autofill data";
            return;
        }
        file.write(payload);
        return;
    }

    QSaveFile file(fileName);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "Unable to open" << fileName
                   << "to store autofill data";
        return;
    }
    file.write(sealed);
    if (!file.commit())
        qWarning() << "Unable to commit" << fileName;
    QFile::setPermissions(fileName, QFile::ReadUser | QFile::WriteUser);
#endif // ARORA_RUSTCORE
}

void AutoFillManager::loadFormData()
{
    QString fileName = autoFillDataFile();

#ifdef ARORA_RUSTCORE
    rustCoreEnsureDataDir();
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        SecureStore::ensureUnlocked(nullptr);

    // One-shot import: when the Rust store does not exist yet the
    // legacy file is decoded by the original reader, replayed into
    // the core and retired.  An undecodable blob is left in place
    // for the next attempt (same outcome as the no-Rust path: an
    // empty list plus a warning).
    if (rc_autofill_store_present() == 0 && QFile::exists(fileName)) {
        QList<Form> legacy;
        QFile file(fileName);
        const bool decoded = file.open(QFile::ReadOnly)
            && decodeLegacyForms(file.readAll(), &legacy);
        if (decoded) {
            const QByteArray json = formsToJson(legacy);
            const RcStatus status = rc_autofill_set_forms(
                reinterpret_cast<const uint8_t *>(json.constData()),
                size_t(json.size()));
            if (status == RC_OK) {
                retireLegacyFile(fileName);
            } else {
                char *error = rc_last_error_message();
                qWarning() << "AutoFillManager: legacy import could not"
                              " be stored —" << fileName
                           << "stays in place:"
                           << QString::fromUtf8(error ? error : "");
                rc_string_free(error);
            }
            m_forms = legacy;
            return;
        }
        qWarning() << "AutoFillManager: cannot decode" << fileName
                   << "(key missing or file tampered)";
    }

    char *json = rc_autofill_forms();
    if (!json) {
        char *error = rc_last_error_message();
        qWarning() << "AutoFillManager: cannot read the autofill store:"
                   << QString::fromUtf8(error ? error : "");
        rc_string_free(error);
        return;
    }
    m_forms = formsFromJson(QByteArray(json));
    rc_string_free(json);
    return;
#else
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly))
        return;

    const QByteArray raw = file.readAll();
    if (SecureStore::isSealed(raw)) {
        bool ok = false;
        const QByteArray payload = SecureStore::open(raw, &ok);
        if (!ok) {
            qWarning() << "AutoFillManager: cannot decrypt" << fileName
                       << "(key missing or file tampered)";
            return;
        }
        QDataStream stream(payload);
        StreamingUtils::readBoundedList(stream, m_forms);
        return;
    }

    // Legacy plaintext store from the Qt4 era — it is migrated to the
    // sealed format on the next save.  The bounded read guards against
    // a corrupt count prefix in the unauthenticated legacy file.
    QDataStream stream(raw);
    StreamingUtils::readBoundedList(stream, m_forms);
#endif // ARORA_RUSTCORE
}

// Builds the injected bundle for a document at url and re-arms the
// page's capture bridge to match.  Private browsing is per-profile:
// pages on the off-the-record profile get the fill pass (parity with
// the old global private mode) but no submit capture, and their
// bridge drops reports.
// The channel bridge lives on the engine page (WebPage registers it
// through QWebChannel) — engine-bound plumbing reached through the
// adapter's escape hatch while callers stay on the interface.
static QWebEnginePage *enginePageFor(Engine::Page *page)
{
    WebEnginePageAdapter *adapter = WebEnginePageAdapter::of(page);
    return adapter ? adapter->webEnginePage() : nullptr;
}

bool AutoFillManager::captureEnabledForPage(Engine::Page *page) const
{
    QWebEnginePage *enginePage = enginePageFor(page);
    return enginePage && !page->isOffTheRecord()
        && enginePage->findChild<AutoFillBridge *>();
}

QString AutoFillManager::scriptForPage(Engine::Page *page,
        const QUrl &url)
{
    const bool capture = captureEnabledForPage(page);
    QWebEnginePage *enginePage = enginePageFor(page);
    AutoFillBridge *bridge =
        enginePage ? enginePage->findChild<AutoFillBridge *>() : nullptr;
    // SEC08: a fresh token per load — the injected script passes it
    // back inside its closure, and the bridge rejects reports without
    // it so page script cannot mint submits of its own.
    const QString token = capture
            ? QUuid::createUuid().toString(QUuid::WithoutBraces)
            : QString();
    if (bridge)
        bridge->setPageInfo(url, capture, token);

    return autoFillScript(fetchForms(stripUrl(url)), capture, token);
}

static void replacePageScript(Engine::Page *page, const QString &name,
        const QString &source)
{
    page->removeScript(name);
    if (source.isEmpty())
        return;
    // worldId 0 is the main world — NOT the QWebEngineScript default
    // (ApplicationWorld): the bundle must run where the page's
    // HTMLFormElement.prototype and the __aroraChannel bootstrap live.
    page->insertScript(Engine::Script{
        name, source, Engine::InjectionPoint::DocumentReady,
        0 /* main world */, false });
}

void AutoFillManager::attachToPage(Engine::Page *page)
{
    if (!page)
        return;
    const QString script = scriptForPage(page, page->url());
    if (!script.isEmpty())
        page->runJavaScript(script);
}

void AutoFillManager::scheduleOnPage(Engine::Page *page,
        const QUrl &url)
{
    if (!page)
        return;
    const QString source = scriptForPage(page, url);
    replacePageScript(page, QLatin1String("arora:autofill"), source);
}

QString AutoFillManager::autoFillScript(const QList<Form> &forms,
        bool capture, const QString &reportToken) const
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
    injected.replace(QLatin1String("REPORT_TOKEN"), reportToken);
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
    StreamingUtils::readBoundedList(in, form.elements);
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
