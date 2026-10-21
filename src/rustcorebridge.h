/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef RUSTCOREBRIDGE_H
#define RUSTCOREBRIDGE_H

#include <qobject.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qvariant.h>

// Qt side of the rustcore change-callback -> Qt-signal bridge
// (RCORE01).  The crate invokes one C callback with a topic string;
// RustCoreBridge re-emits it as a queued storeChanged() signal so a
// Rust-side caller on any thread lands safely on the GUI thread.
//
// Topics so far: "credentials" (the rc_cred_* map changed),
// "bookmarks" and "history" (the RCORE02 stores), "autofill" (the
// RCORE05 record set).
// Compiled only under CONFIG+=rustcore (ARKTOS_RUSTCORE).
class RustCoreBridge : public QObject
{
    Q_OBJECT

public:
    // Leaky app-lifetime singleton, created on demand — call only when
    // a QCoreApplication exists.
    static RustCoreBridge *instance();

signals:
    void storeChanged(const QString &topic);

private:
    explicit RustCoreBridge(QObject *parent = nullptr);
    static void trampoline(const char *topic, void *userdata);
};

// Points the crate at the application data dir — the same
// idempotent call the SecureStore shim makes before every op, so
// test-mode data-dir switches propagate (RCORE02).
void rustCoreEnsureDataDir();

// MPROF02: points the crate at a SPECIFIC profile's dir —
// profiles/<id>/ instead of the active profile's.  The rust store is
// process-global: switching dirs drops its cached keys and the
// decoded credential/autofill maps (per rc_set_data_dir), and every
// DirCache re-stamps, so alternating profiles is correct but
// expensive — callers anchor per operation batch, never per page
// load.  An empty or non-slug id resolves to the default profile,
// matching BrowserPaths::profileDataDir.
void rustCoreEnsureDataDirFor(const QString &profileId);

struct RcSettings;

// RC02: thin Qt wrapper over the rc_settings_* typed per-profile
// store — the QSettings-shaped surface RC08 swaps in for
// ProfileSettings' persistence.  The scope binds at construction:
// a profile slug opens <data root>/profiles/<id>/settings.json and
// auto-imports the legacy settings.ini on first open; an empty id
// opens the global scope (<data root>/settings.json).
//
// Mirrors the C ABI 1:1: value() reconstructs a QVariant from the
// store's typed JSON (string/bool/int/double/bytes/string-list),
// setValue() maps the QVariant back, and QVariant-style conversion
// covers the ini-imported string scalars.
class RustCoreSettings
{
public:
    // Empty profileId selects the global scope.
    explicit RustCoreSettings(const QString &profileId = QString());
    ~RustCoreSettings();

    RustCoreSettings(const RustCoreSettings &) = delete;
    RustCoreSettings &operator=(const RustCoreSettings &) = delete;

    bool isValid() const;

    QVariant value(const QString &key,
                   const QVariant &defaultValue = QVariant()) const;
    void setValue(const QString &key, const QVariant &value);
    void remove(const QString &key);
    bool contains(const QString &key) const;
    // Keys under prefix (boundary match: prefix and prefix/…; an
    // empty prefix lists everything).
    QStringList allKeys(const QString &prefix = QString()) const;
    void sync();

    // One-shot merge of a legacy QSettings ini at an explicit path —
    // the lane for the global QSettings file, which lives outside
    // the data root.  Profile scope keeps isProfileKey()-classified
    // keys only; global scope keeps the complement.
    bool importLegacyIni(const QString &iniPath);

    QString profileId() const;

private:
    RcSettings *m_h;
    QString m_profileId;
};

#endif // RUSTCOREBRIDGE_H
