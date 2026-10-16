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

#include <qset.h>
#include <qurl.h>
#include <qvariantmap.h>

class QDataStream;
class AutoSaver;
namespace Engine { class Page; }

// Per-page bridge exposed to JavaScript as "aroraAutofill" through the
// page's QWebChannel (registered in WebPage::init).  The injected
// autofill.js reports submitted forms through submitForm(); the bridge
// tags each report with the url the page had when the hook was
// installed so a page cannot forge form data for a different origin.
// SEC24: the channel lives in AroraChannelWorld — page script cannot
// reach this object at all.  The SEC08 per-load token stays as the
// second wall: it lives only inside the injected script's closure in
// that world, so a report without it is still dropped.
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

    // ADDR01: a user-entered postal/contact address (the prefs page's
    // "Saved addresses" section).  country stores the ISO territory
    // code (QLocale::territoryToCode); the UI resolves it back to a
    // display name.  An address is valid when it carries at least a
    // name or a street — the same gate the editor applies to Save.
    class Address {
    public:
        bool isValid() const
        { return !name.trimmed().isEmpty() || !street.trimmed().isEmpty(); }
        bool operator==(const Address &other) const
        {
            return country == other.country && name == other.name
                && organization == other.organization
                && street == other.street && city == other.city
                && state == other.state && zip == other.zip
                && phone == other.phone && email == other.email;
        }
        bool operator!=(const Address &other) const
        { return !(*this == other); }
        static void load(QDataStream &in, Address &address);
        static void save(QDataStream &out, const Address &address);

        QString country;
        QString name;
        QString organization;
        QString street;
        QString city;
        QString state;
        QString zip;
        QString phone;
        QString email;
        // ADDR02: UTC seconds the address last filled a submitted
        // form — the most-recently-used ordering the page fill picks
        // by.  Metadata, not identity, so operator== excludes it; it
        // rides a parallel tail in the store (see saveFormData).
        qint64 lastUsed = 0;
    };

    // Lazy qApp-owned singleton (same pattern as HistoryManager /
    // BookmarksManager / AdBlockManager).  BrowserApplication delegates
    // here; the manager reads the profile-independent store itself.
    static AutoFillManager *instance();
    // MPROF02: the store for a specific profile — one instance per
    // registry id, qApp-owned.  Each instance's autofill.dat /
    // rust autofill-store lives under profiles/<id>/ and seals under
    // that profile's SecureStore key.
    static AutoFillManager *instanceFor(const QString &profileId);
    QString profileId() const { return m_profileId; }

    // profileId names the registry profile whose store this manager
    // owns (MPROF02); an empty or non-slug id binds the ambient
    // (focused window's) profile.
    AutoFillManager(QObject *parent = nullptr,
                    const QString &profileId = QString());
    ~AutoFillManager();

    void loadSettings();

    // SEC16: arms the same bundle as a named per-page QWebEngineScript
    // at DocumentReady keyed to the navigation's target url — the
    // injection rides Chromium's own script pipeline instead of
    // competing with paint-gated subresource scheduling after load.
    void scheduleOnPage(Engine::Page *page, const QUrl &url);

    void setForms(const QList<Form> &forms);
    QList<Form> forms() const;

    // Saved addresses — mutated from the AutoFill prefs page; every
    // write emits autoFillChanged() so the autosave picks it up.
    // setAddresses preserves lastUsed by content match (callers keep
    // any nonzero stamp they set themselves) so prefs round trips
    // don't lose the MRU order.
    void setAddresses(const QList<Address> &addresses);
    QList<Address> addresses() const;
    void addAddress(const Address &address);
    void removeAddress(int index);
    // The address a page fill should offer: the most recently used,
    // ties broken toward the most recently added.
    const Address *preferredAddress() const;
    // Content fingerprint identifying one stored entry — the injected
    // script echoes it back on submit so the use mark lands on the
    // address the page actually filled, not a shifted index.
    static QString addressId(const Address &address);
    void markAddressUsed(const QString &addressId);
    // Clear Private Data wipe: drops forms, addresses and the
    // session's "never save" sites, then writes the store immediately
    // instead of leaving the purge pending on the autosave timer.
    void clearData();

public slots:
    // Invoked by the page's AutoFillBridge when the injected script
    // reports a submitted form.
    void formSubmitted(const QUrl &pageUrl, const QString &reportedUrl,
                       const QVariantMap &formData);

private slots:
    void save() const;

private:
    static QUrl stripUrl(const QUrl &url);
    QString autoFillDataFile() const;
    bool allowedToAutoFill(bool password) const;
    QList<AutoFillManager::Form> fetchForms(const QUrl &url) const;
    QString scriptForPage(Engine::Page *page, const QUrl &url);
    bool captureEnabledForPage(Engine::Page *page) const;
    QString autoFillScript(const QList<Form> &forms,
                           const Address *address, bool capture,
                           const QString &reportToken) const;
    bool promptToSave(const QUrl &url);
    // ADDR02: submit-time address handling — turns the report's
    // detected address fields into a candidate and, when it is
    // address-shaped and new, opens the pre-filled editor as the
    // "save this address?" prompt.
    void maybeSaveAddress(const QUrl &url, const QVariantMap &formData);
    void promptToSaveAddress(const Address &candidate);

    void saveFormData() const;
    void loadFormData();

    QString m_profileId;
    bool m_savePasswordForms;
    bool m_allowAutoCompleteOff;

    QList<Form> m_forms;
    QList<Address> m_addresses;
    QList<QUrl> m_never;
    // Stripped urls already shown the address-save prompt this
    // session — the editor has no "never for this site" button, so
    // declining must not re-ask on every checkout submit.
    QSet<QString> m_addressPrompted;
    AutoSaver *m_saveTimer;
};

QDataStream &operator<<(QDataStream &, const AutoFillManager::Form &form);
QDataStream &operator>>(QDataStream &, AutoFillManager::Form &form);
QDataStream &operator<<(QDataStream &, const AutoFillManager::Address &address);
QDataStream &operator>>(QDataStream &, AutoFillManager::Address &address);

#endif // AUTOFILLMANAGER_H
