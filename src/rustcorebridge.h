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

// Qt side of the rustcore change-callback -> Qt-signal bridge
// (RCORE01).  The crate invokes one C callback with a topic string;
// RustCoreBridge re-emits it as a queued storeChanged() signal so a
// Rust-side caller on any thread lands safely on the GUI thread.
//
// Topics so far: "credentials" (the rc_cred_* map changed),
// "bookmarks" and "history" (the RCORE02 stores).
// Compiled only under CONFIG+=rustcore (ARORA_RUSTCORE).
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

#endif // RUSTCOREBRIDGE_H
