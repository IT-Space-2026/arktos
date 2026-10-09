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

#include "rustcorebridge.h"

#include "browserpaths.h"

#include <qfileinfo.h>

#include <rustcore.h>

RustCoreBridge *RustCoreBridge::instance()
{
    static RustCoreBridge *self = new RustCoreBridge;
    return self;
}

RustCoreBridge::RustCoreBridge(QObject *parent)
    : QObject(parent)
{
    rc_set_change_callback(&RustCoreBridge::trampoline, this);
}

void RustCoreBridge::trampoline(const char *topic, void *userdata)
{
    RustCoreBridge *self = static_cast<RustCoreBridge *>(userdata);
    const QString t = QString::fromUtf8(topic ? topic : "");
    // Rust may call from any thread; hop to the object's thread.
    QMetaObject::invokeMethod(self, [self, t]() {
        emit self->storeChanged(t);
    }, Qt::QueuedConnection);
}

void rustCoreEnsureDataDir()
{
    // Cheap and idempotent — call per operation so test-mode
    // data-dir switches propagate.
    const QByteArray dir =
        QFileInfo(BrowserPaths::dataFilePath(QLatin1String(".rc")))
            .absolutePath().toUtf8();
    rc_set_data_dir(dir.constData());
}
