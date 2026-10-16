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
#include "profilesettings.h"

#include "addressdialog.h"
#include "autosaver.h"
#include "browserpaths.h"
#include "browserprofile.h"
#include "engineinterface.h"
#include "securestore.h"
#include "startupprofile.h"
#include "streamingutils.h"
#include "webenginebackend.h"

#ifdef ARORA_RUSTCORE
#include "rustcorebridge.h"

#include <rustcore.h>
#endif

#include <qcryptographichash.h>
#include <qdatastream.h>
#include <qdatetime.h>
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
    // The bridge is parented to the WebPage — the submit lands in
    // the profile the page runs on, not necessarily the active one.
    QWebEnginePage *page = qobject_cast<QWebEnginePage *>(parent());
    const QString profileId = BrowserProfile::profileIdFor(
        page ? page->profile() : nullptr);
    AutoFillManager::instanceFor(profileId)
        ->formSubmitted(m_pageUrl, reportedUrl, formData);
}

AutoFillManager *AutoFillManager::instance()
{
    return instanceFor(BrowserPaths::activeProfileId());
}

AutoFillManager *AutoFillManager::instanceFor(const QString &profileId)
{
    const QString id = BrowserPaths::isProfileId(profileId)
            ? profileId : BrowserPaths::activeProfileId();
    static QHash<QString, QPointer<AutoFillManager>> managers;
    QPointer<AutoFillManager> &manager = managers[id];
    if (!manager)
        manager = new AutoFillManager(qApp, id);
    return manager;
}

AutoFillManager::AutoFillManager(QObject *parent,
                                 const QString &profileId)
    : QObject(parent)
    , m_profileId(BrowserPaths::isProfileId(profileId)
                      ? profileId : BrowserPaths::activeProfileId())
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

void AutoFillManager::setAddresses(const QList<Address> &addresses)
{
    // Carry lastUsed across prefs-page round trips: entries match by
    // content (operator== ignores the use stamp), so editing the list
    // keeps the MRU order.  A nonzero stamp the caller set itself is
    // kept as-is.
    const QList<Address> old = m_addresses;
    m_addresses = addresses;
    QSet<int> consumed;
    for (Address &address : m_addresses) {
        if (address.lastUsed)
            continue;
        for (int i = 0; i < old.count(); ++i) {
            if (!consumed.contains(i) && old.at(i) == address) {
                address.lastUsed = old.at(i).lastUsed;
                consumed.insert(i);
                break;
            }
        }
    }
    emit autoFillChanged();
}

QList<AutoFillManager::Address> AutoFillManager::addresses() const
{
    return m_addresses;
}

const AutoFillManager::Address *AutoFillManager::preferredAddress() const
{
    // One saved address fills directly; with several, the most
    // recently used wins — lastUsed ties (e.g. all fresh entries)
    // fall to the most recently added, the one the user just typed.
    const Address *best = nullptr;
    for (const Address &address : m_addresses) {
        if (!address.isValid())
            continue;
        if (!best || address.lastUsed >= best->lastUsed)
            best = &address;
    }
    return best;
}

QString AutoFillManager::addressId(const Address &address)
{
    QByteArray key;
    for (const QString &field : { address.country, address.name,
            address.organization, address.street, address.city,
            address.state, address.zip, address.phone, address.email }) {
        key += field.toUtf8();
        key += '\x1f';
    }
    return QString::fromLatin1(QCryptographicHash::hash(
        key, QCryptographicHash::Sha1).toHex());
}

void AutoFillManager::markAddressUsed(const QString &addressId)
{
    if (addressId.isEmpty())
        return;
    for (Address &address : m_addresses) {
        if (AutoFillManager::addressId(address) == addressId) {
            address.lastUsed = QDateTime::currentSecsSinceEpoch();
            emit autoFillChanged();
            return;
        }
    }
}

void AutoFillManager::addAddress(const Address &address)
{
    m_addresses.append(address);
    emit autoFillChanged();
}

void AutoFillManager::removeAddress(int index)
{
    if (index < 0 || index >= m_addresses.count())
        return;
    m_addresses.removeAt(index);
    emit autoFillChanged();
}

void AutoFillManager::clearData()
{
    m_forms.clear();
    m_addresses.clear();
    m_never.clear();
    emit autoFillChanged();
    saveFormData();
}

void AutoFillManager::loadSettings()
{
    ProfileSettings settings(m_profileId);
    settings.beginGroup(QLatin1String("autofill"));
    m_savePasswordForms = settings.value(QLatin1String("passwordForms"), m_savePasswordForms).toBool();
    m_allowAutoCompleteOff = settings.value(QLatin1String("ignoreAutoCompleteOff"), m_allowAutoCompleteOff).toBool();
}

QString AutoFillManager::autoFillDataFile() const
{
    return BrowserPaths::profileDataFilePath(
        m_profileId, QLatin1String("autofill.dat"));
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

// ADDR01/02: the saved-address list shares the sealed store as an
// appended payload section — the same record set the legacy
// autofill.dat tail carries.
static QByteArray addressesToJson(
    const QList<AutoFillManager::Address> &addresses)
{
    QJsonArray all;
    for (const AutoFillManager::Address &address : addresses) {
        QJsonObject addressJson;
        addressJson[QLatin1String("country")] = address.country;
        addressJson[QLatin1String("name")] = address.name;
        addressJson[QLatin1String("organization")] =
            address.organization;
        addressJson[QLatin1String("street")] = address.street;
        addressJson[QLatin1String("city")] = address.city;
        addressJson[QLatin1String("state")] = address.state;
        addressJson[QLatin1String("zip")] = address.zip;
        addressJson[QLatin1String("phone")] = address.phone;
        addressJson[QLatin1String("email")] = address.email;
        addressJson[QLatin1String("last_used")] =
            double(address.lastUsed);
        all.append(addressJson);
    }
    return QJsonDocument(all).toJson(QJsonDocument::Compact);
}

static QList<AutoFillManager::Address> addressesFromJson(
    const QByteArray &json)
{
    QList<AutoFillManager::Address> addresses;
    const QJsonArray all = QJsonDocument::fromJson(json).array();
    for (const QJsonValue &value : all) {
        const QJsonObject addressJson = value.toObject();
        AutoFillManager::Address address;
        address.country =
            addressJson.value(QLatin1String("country")).toString();
        address.name =
            addressJson.value(QLatin1String("name")).toString();
        address.organization =
            addressJson.value(QLatin1String("organization")).toString();
        address.street =
            addressJson.value(QLatin1String("street")).toString();
        address.city =
            addressJson.value(QLatin1String("city")).toString();
        address.state =
            addressJson.value(QLatin1String("state")).toString();
        address.zip =
            addressJson.value(QLatin1String("zip")).toString();
        address.phone =
            addressJson.value(QLatin1String("phone")).toString();
        address.email =
            addressJson.value(QLatin1String("email")).toString();
        address.lastUsed = qint64(
            addressJson.value(QLatin1String("last_used")).toDouble());
        if (address.isValid())
            addresses.append(address);
    }
    return addresses;
}

// Decodes raw autofill.dat contents — a sealed ARSEC1 blob or the
// Qt4-era plaintext QDataStream — through the original bounded
// reader.  The payload is [forms] then the optional ADDR01/02
// [addresses][lastUsed] tails; a forms-only file imports with an
// empty address list.  Returns false on an unopenable/corrupt blob.
static bool decodeLegacyPayload(const QString &profileId,
                                const QByteArray &raw,
                                QList<AutoFillManager::Form> *formsOut,
                                QList<AutoFillManager::Address> *addrsOut)
{
    QByteArray payload = raw;
    if (SecureStore::isSealed(raw)) {
        bool ok = false;
        payload = SecureStore::open(profileId, raw, &ok);
        if (!ok)
            return false;
    }
    QList<AutoFillManager::Form> forms;
    QDataStream stream(payload);
    StreamingUtils::readBoundedList(stream, forms);
    if (stream.status() != QDataStream::Ok)
        return false;
    QList<AutoFillManager::Address> addresses;
    if (!stream.atEnd())
        StreamingUtils::readBoundedList(stream, addresses);
    if (stream.status() != QDataStream::Ok)
        return false;
    if (!stream.atEnd()) {
        QList<qint64> lastUsed;
        StreamingUtils::readBoundedList(stream, lastUsed);
        for (int i = 0; i < lastUsed.count()
                && i < addresses.count(); ++i)
            addresses[i].lastUsed = lastUsed.at(i);
    }
    if (stream.status() != QDataStream::Ok)
        return false;
    *formsOut = forms;
    *addrsOut = addresses;
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
    rustCoreEnsureDataDirFor(m_profileId);
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        SecureStore::ensureUnlocked(m_profileId, nullptr);

    const QByteArray json = formsToJson(m_forms);
    const RcStatus status = rc_autofill_set_forms(
        reinterpret_cast<const uint8_t *>(json.constData()),
        size_t(json.size()));
    // ADDR01/02: the saved-address list rides the same sealed payload
    // — a store that only persisted forms would silently drop the
    // addresses the prefs page edits.
    const QByteArray addrJson = addressesToJson(m_addresses);
    const RcStatus addrStatus = rc_autofill_set_addresses(
        reinterpret_cast<const uint8_t *>(addrJson.constData()),
        size_t(addrJson.size()));
    if (status == RC_OK && addrStatus == RC_OK) {
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
        // ADDR01: [forms][addresses] — the address list is appended
        // after the forms so a pre-ADDR01 build still reads its Form
        // list untouched and simply ignores the trailing bytes.
        stream << m_forms;
        stream << m_addresses;
        // ADDR02: per-address MRU stamps ride a third tail — a
        // parallel QList<qint64> rather than a field inside
        // Address::save, which would misalign every pre-ADDR02
        // build's reads mid-list.  Older builds stop after the
        // addresses and ignore it.
        QList<qint64> lastUsed;
        lastUsed.reserve(m_addresses.count());
        for (const Address &address : m_addresses)
            lastUsed.append(address.lastUsed);
        stream << lastUsed;
    }

    const QByteArray sealed = SecureStore::seal(m_profileId, payload);
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
    rustCoreEnsureDataDirFor(m_profileId);
    if (rc_passphrase_enabled() && !rc_is_unlocked())
        SecureStore::ensureUnlocked(m_profileId, nullptr);

    // One-shot import: when the Rust store does not exist yet the
    // legacy file is decoded by the original reader, replayed into
    // the core and retired.  An undecodable blob is left in place
    // for the next attempt (same outcome as the no-Rust path: an
    // empty list plus a warning).
    if (rc_autofill_store_present() == 0 && QFile::exists(fileName)) {
        QList<Form> legacy;
        QList<Address> legacyAddresses;
        QFile file(fileName);
        const bool decoded = file.open(QFile::ReadOnly)
            && decodeLegacyPayload(m_profileId, file.readAll(), &legacy,
                                   &legacyAddresses);
        if (decoded) {
            const QByteArray json = formsToJson(legacy);
            const RcStatus status = rc_autofill_set_forms(
                reinterpret_cast<const uint8_t *>(json.constData()),
                size_t(json.size()));
            // ADDR01/02: the legacy file's address tail imports with
            // the forms — the sealed store holds both from now on.
            const QByteArray addrJson = addressesToJson(legacyAddresses);
            const RcStatus addrStatus = rc_autofill_set_addresses(
                reinterpret_cast<const uint8_t *>(addrJson.constData()),
                size_t(addrJson.size()));
            if (status == RC_OK && addrStatus == RC_OK) {
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
            m_addresses = legacyAddresses;
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

    // ADDR01/02: the address list shares the sealed payload — read it
    // under the same unlock.
    char *addrJson = rc_autofill_addresses();
    if (!addrJson) {
        char *error = rc_last_error_message();
        qWarning() << "AutoFillManager: cannot read the autofill"
                      " address store:"
                   << QString::fromUtf8(error ? error : "");
        rc_string_free(error);
        return;
    }
    m_addresses = addressesFromJson(QByteArray(addrJson));
    rc_string_free(addrJson);
    return;
#else
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly))
        return;

    const QByteArray raw = file.readAll();

    // Reads [forms] then the optional [addresses] tail — a payload
    // written before ADDR01 ends after the forms, which yields an
    // empty address list (the bounded read clears the list on any
    // truncation too, so a corrupt tail can't leave a partial one).
    auto decodePayload = [this](QDataStream &stream) {
        StreamingUtils::readBoundedList(stream, m_forms);
        m_addresses.clear();
        if (stream.status() == QDataStream::Ok && !stream.atEnd())
            StreamingUtils::readBoundedList(stream, m_addresses);
        // ADDR02: optional MRU tail — absent on ADDR01-era files,
        // which decode with all-zero stamps.
        if (stream.status() == QDataStream::Ok && !stream.atEnd()) {
            QList<qint64> lastUsed;
            StreamingUtils::readBoundedList(stream, lastUsed);
            for (int i = 0; i < lastUsed.count()
                    && i < m_addresses.count(); ++i)
                m_addresses[i].lastUsed = lastUsed.at(i);
        }
    };

    if (SecureStore::isSealed(raw)) {
        bool ok = false;
        const QByteArray payload = SecureStore::open(m_profileId, raw, &ok);
        if (!ok) {
            qWarning() << "AutoFillManager: cannot decrypt" << fileName
                       << "(key missing or file tampered)";
            return;
        }
        QDataStream stream(payload);
        decodePayload(stream);
        return;
    }

    // Legacy plaintext store from the Qt4 era — it is migrated to the
    // sealed format on the next save.  The bounded read guards against
    // a corrupt count prefix in the unauthenticated legacy file.
    QDataStream stream(raw);
    decodePayload(stream);
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

    // ADDR02: address fill is gated exactly like the stored-form fill
    // — armed on every named-profile page, never on off-the-record
    // ones (ADDR02's privacy gate is stricter than the form fill's:
    // no address data leaves the store for a private page at all).
    const Address *address = page->isOffTheRecord()
            ? nullptr : preferredAddress();

    return autoFillScript(fetchForms(stripUrl(url)), address, capture, token);
}

static void replacePageScript(Engine::Page *page, const QString &name,
        const QString &source)
{
    page->removeScript(name);
    if (source.isEmpty())
        return;
    // SEC24: the bundle runs in AroraChannelWorld — the isolated
    // world the page's QWebChannel is bound to — where the
    // __aroraChannel bootstrap lives and page script cannot follow.
    page->insertScript(Engine::Script{
        name, source, Engine::InjectionPoint::DocumentReady,
        Engine::AroraChannelWorld, false });
}

// form.submit() bypasses the 'submit' DOM event, so scripted
// submissions need the page's own HTMLFormElement.prototype wrapped
// — and a prototype patch only sticks in the world that installs it.
// autofill.js lives in the isolated channel world (SEC24), so the
// wrapper is armed as a separate main-world script that relays each
// call as an 'arora:formsubmit' event; the isolated-side listener
// serializes the real form.  Page script can forge that event, but
// the forged trigger only re-serializes DOM the attacker already
// owns — the token never leaves the channel world.
static const char s_submitShimSource[] =
    "(function () {"
    "    if (window.__aroraAutofillSubmitShim) return;"
    "    window.__aroraAutofillSubmitShim = true;"
    "    var nativeSubmit = HTMLFormElement.prototype.submit;"
    "    HTMLFormElement.prototype.submit = function () {"
    "        try {"
    "            this.dispatchEvent(new Event('arora:formsubmit',"
    "                {bubbles: true}));"
    "        } catch (e) {}"
    "        return nativeSubmit.call(this);"
    "    };"
    "}())";

void AutoFillManager::scheduleOnPage(Engine::Page *page,
        const QUrl &url)
{
    if (!page)
        return;
    const QString source = scriptForPage(page, url);
    replacePageScript(page, QLatin1String("arora:autofill"), source);

    const bool capture = !source.isEmpty()
        && captureEnabledForPage(page);
    if (capture) {
        page->insertScript(Engine::Script{
            QLatin1String("arora:autofill-submit"),
            QString::fromLatin1(s_submitShimSource),
            Engine::InjectionPoint::DocumentReady, 0 /* main world */,
            false });
    } else {
        page->removeScript(QLatin1String("arora:autofill-submit"));
    }
}

QString AutoFillManager::autoFillScript(const QList<Form> &forms,
        const Address *address, bool capture,
        const QString &reportToken) const
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

    // ADDR02: at most ONE address is handed to the page — the MRU
    // pick, tagged with the content id the report echoes back.  The
    // whole list never enters the page world (same closure discipline
    // as FORMS_JSON, but minimal disclosure keeps it to the pick).
    QString addressJson = QStringLiteral("null");
    if (address) {
        QJsonObject addressObject;
        addressObject[QLatin1String("id")] = addressId(*address);
        addressObject[QLatin1String("country")] = address->country;
        addressObject[QLatin1String("name")] = address->name;
        addressObject[QLatin1String("organization")] = address->organization;
        addressObject[QLatin1String("street")] = address->street;
        addressObject[QLatin1String("city")] = address->city;
        addressObject[QLatin1String("state")] = address->state;
        addressObject[QLatin1String("zip")] = address->zip;
        addressObject[QLatin1String("phone")] = address->phone;
        addressObject[QLatin1String("email")] = address->email;
        addressJson = QString::fromUtf8(
            QJsonDocument(addressObject).toJson(QJsonDocument::Compact));
    }

    QString injected = script;
    injected.replace(QLatin1String("FORMS_JSON"),
                     QString::fromUtf8(QJsonDocument(formsJson).toJson(QJsonDocument::Compact)));
    injected.replace(QLatin1String("ADDRESS_JSON"), addressJson);
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

    // ADDR02: the same report carries the used-address mark and a
    // detected address candidate — handled before the password-form
    // pipeline, which early-returns on anything it cannot store.
    markAddressUsed(formData[QLatin1String("usedAddressId")].toString());
    maybeSaveAddress(url, formData);

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

void AutoFillManager::maybeSaveAddress(const QUrl &url,
        const QVariantMap &formData)
{
    // Same gate as password capture: the "User names and passwords"
    // autofill switch is the only master toggle, and it governs the
    // save-address prompt too (fill itself is ungated, like forms).
    if (!m_savePasswordForms)
        return;
    // The AddressDialog has no "never for this site" button — decline
    // once per site per session instead of re-asking every submit.
    const QString key = url.toString();
    if (m_addressPrompted.contains(key))
        return;

    Address candidate;
    int filled = 0;
    const QVariantList fields =
        formData[QLatin1String("addressFields")].toList();
    for (const QVariant &entry : fields) {
        const QVariantMap map = entry.toMap();
        // Same autocomplete="off" rule as the Form path: when
        // ignoreAutoCompleteOff is set the opted-out fields are
        // dropped from what gets stored.
        if (m_allowAutoCompleteOff
            && map[QLatin1String("off")].toBool())
            continue;
        const QString member = map[QLatin1String("member")].toString();
        const QString value =
            map[QLatin1String("value")].toString().trimmed();
        if (value.isEmpty())
            continue;
        ++filled;
        // autofill.js reports canonical member names; split-name and
        // multi-line street fields merge back into one member here.
        if (member == QLatin1String("nameFirst")) {
            candidate.name = candidate.name.isEmpty()
                ? value : value + QLatin1Char(' ') + candidate.name;
        } else if (member == QLatin1String("nameLast")) {
            candidate.name = candidate.name.isEmpty()
                ? value : candidate.name + QLatin1Char(' ') + value;
        } else if (member.startsWith(QLatin1String("street"))
                   && member != QLatin1String("street")) {
            if (!candidate.street.isEmpty())
                candidate.street += QLatin1Char('\n');
            candidate.street += value;
        } else if (member == QLatin1String("name")) {
            candidate.name = value;
        } else if (member == QLatin1String("organization")) {
            candidate.organization = value;
        } else if (member == QLatin1String("street")) {
            candidate.street = value;
        } else if (member == QLatin1String("city")) {
            candidate.city = value;
        } else if (member == QLatin1String("state")) {
            candidate.state = value;
        } else if (member == QLatin1String("zip")) {
            candidate.zip = value;
        } else if (member == QLatin1String("phone")) {
            candidate.phone = value;
        } else if (member == QLatin1String("email")) {
            candidate.email = value;
        } else if (member == QLatin1String("country")) {
            candidate.country = value;
        }
    }
    // "Address-shaped" means at least two usable fields where at
    // least one is real postal evidence (street/city/zip) — a bare
    // name+email contact form doesn't earn the prompt.
    const bool postal = !candidate.street.isEmpty()
        || !candidate.city.isEmpty() || !candidate.zip.isEmpty();
    if (filled < 2 || !postal)
        return;

    // Don't re-save what is already stored — near-dup on the
    // identity triple name+street+zip (case-insensitive).
    for (const Address &address : m_addresses) {
        if (!address.name.compare(candidate.name, Qt::CaseInsensitive)
            && !address.street.compare(candidate.street,
                                       Qt::CaseInsensitive)
            && !address.zip.compare(candidate.zip, Qt::CaseInsensitive))
            return;
    }

    m_addressPrompted.insert(key);
    promptToSaveAddress(candidate);
}

void AutoFillManager::promptToSaveAddress(const Address &candidate)
{
    // The pre-filled editor IS the "save this address?" prompt: the
    // user lands in the same dialog the prefs page's Add button
    // opens and can fix anything before Save.
    AddressDialog dialog;
    dialog.setAddress(candidate);
    if (dialog.exec() == QDialog::Accepted
        && dialog.address().isValid())
        addAddress(dialog.address());
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

QDataStream &operator>>(QDataStream &in, AutoFillManager::Address &address)
{
    AutoFillManager::Address::load(in, address);
    return in;
}

QDataStream &operator<<(QDataStream &out, const AutoFillManager::Address &address)
{
    AutoFillManager::Address::save(out, address);
    return out;
}

void AutoFillManager::Address::load(QDataStream &in, AutoFillManager::Address &address)
{
    in >> address.country;
    in >> address.name;
    in >> address.organization;
    in >> address.street;
    in >> address.city;
    in >> address.state;
    in >> address.zip;
    in >> address.phone;
    in >> address.email;
}

void AutoFillManager::Address::save(QDataStream &out, const AutoFillManager::Address &address)
{
    out << address.country;
    out << address.name;
    out << address.organization;
    out << address.street;
    out << address.city;
    out << address.state;
    out << address.zip;
    out << address.phone;
    out << address.email;
}
