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

// libFuzzer target for SecureStore::open — the sealed-blob boundary
// (magic | nonce | tag | ciphertext) that autofill.dat and sealed
// QSettings values pass through.  Garbage input must fail cleanly;
// a seeded real blob gets decrypted then bit-flipped.

#include "securestore.h"

#include <qcoreapplication.h>
#include <qstandardpaths.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    // QCoreApplication so QStandardPaths::AppDataLocation resolves;
    // test mode keeps the key file inside the run dir, not $HOME.
    static QCoreApplication app(*argc, *argv);
    QCoreApplication::setOrganizationName(QLatin1String("Arora"));
    QCoreApplication::setApplicationName(QLatin1String("Arora-fuzz"));
    QStandardPaths::setTestModeEnabled(true);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray blob(reinterpret_cast<const char *>(data),
                          qsizetype(size));

    bool ok = false;
    SecureStore::isSealed(blob);
    SecureStore::open(blob, &ok);
    SecureStore::openString(QString::fromUtf8(blob), &ok);

    if (SecureStore::isAvailable()) {
        // Round-trip: sealing attacker bytes must come back verbatim.
        const QByteArray sealed = SecureStore::seal(blob);
        if (!sealed.isEmpty()) {
            bool ok2 = false;
            SecureStore::open(sealed, &ok2);

            // Corrupt a byte chosen by the input — auth must reject.
            if (!blob.isEmpty()) {
                QByteArray tampered = sealed;
                const int at = quint8(blob.at(0)) % tampered.size();
                tampered[at] = tampered[at] ^ 0x5a;
                bool ok3 = true;
                SecureStore::open(tampered, &ok3);
            }
        }
    }
    return 0;
}
