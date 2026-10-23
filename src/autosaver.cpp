/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright 2008 Arora Browser
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

/****************************************************************************
**
** Copyright (C) 2007-2008 Trolltech ASA. All rights reserved.
**
** This file is part of the demonstration applications of the Qt Toolkit.
**
** This file may be used under the terms of the GNU General Public
** License versions 2.0 or 3.0 as published by the Free Software
** Foundation and appearing in the files LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file.  Alternatively you may (at
** your option) use any later version of the GNU General Public
** License if such license has been publicly approved by Trolltech ASA
** (or its successors, if any) and the KDE Free Qt Foundation. In
** addition, as a special exception, Trolltech gives you certain
** additional rights. These rights are described in the Trolltech GPL
** Exception version 1.2, which can be found at
** http://www.trolltech.com/products/qt/gplexception/ and in the file
** GPL_EXCEPTION.txt in this package.
**
** Please review the following information to ensure GNU General
** Public Licensing requirements will be met:
** http://trolltech.com/products/qt/licenses/licensing/opensource/. If
** you are unsure which license is appropriate for your use, please
** review the following information:
** http://trolltech.com/products/qt/licenses/licensing/licensingoverview
** or contact the sales department at sales@trolltech.com.
**
** In addition, as a special exception, Trolltech, as the sole
** copyright holder for Qt Designer, grants users of the Qt/Eclipse
** Integration plug-in the right for the Qt/Eclipse Integration to
** link to functionality provided by Qt Designer and its related
** libraries.
**
** This file is provided "AS IS" with NO WARRANTY OF ANY KIND,
** INCLUDING THE WARRANTIES OF DESIGN, MERCHANTABILITY AND FITNESS FOR
** A PARTICULAR PURPOSE. Trolltech reserves all rights not expressly
** granted herein.
**
** This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
** WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
**
****************************************************************************/

#include "autosaver.h"

#include <qdir.h>
#include <qcoreapplication.h>
#include <qmetaobject.h>

#include <qdebug.h>

#if defined(ARKTOS_RUSTCORE)
#include "browserpaths.h"
#include "rustcorebridge.h"

#include <rustcore.h>
#include <arktos_core.h>

#include <qjsondocument.h>
#include <qjsonobject.h>

#include <string.h>
#endif

// The session-policy defaults — sesspolicy.rs answers these same
// constants when the bound profile stores no "session/" rows, so a
// missing bridge/FFI path and a missing key read identically.
#define AUTOSAVE_IN  (1000 * 3)  // seconds
#define MAXWAIT      (1000 * 15) // seconds

#if defined(ARKTOS_RUSTCORE)
// MD08: one granular call into rustcore's sesspolicy module — the
// session snapshot/restore decision layer (debounce + ceiling,
// dirty-on-what, retention caps, crash-restore, startup fallback)
// lives there now; this saver keeps the timers.  The profile-bound
// AcSession is minted per change — cheap, and a mid-process settings
// write lands without a stale cached document.  With no bridge
// serving the rc_* spelling runs against a bound RcSettings (an
// empty id binds "default" like every other store consumer).  An
// empty verdict maps to the compiled-in constants so an FFI failure
// can never silently skip a save or mistime the debounce.
static QJsonObject sessionPolicyCall(const QString &profileId,
                                     const QJsonObject &request)
{
    AcSession *h = nullptr;
    if (RustCoreBridge *bridge = RustCoreBridge::serving())
        h = bridge->session(profileId);
    RcSettings *s = nullptr;
    if (!h) {
        const QByteArray root = BrowserPaths::dataRoot().toUtf8();
        const QByteArray id = profileId.isEmpty()
            ? QByteArrayLiteral("default") : profileId.toUtf8();
        s = rc_settings_open(root.constData(), id.constData());
        if (!s)
            return QJsonObject();
    }
    const QByteArray json =
        QJsonDocument(request).toJson(QJsonDocument::Compact);
    char *raw = h
        ? ac_session_policy(h,
                            reinterpret_cast<const uint8_t *>(json.constData()),
                            size_t(json.size()))
        : rc_sesspolicy_call(s,
                             reinterpret_cast<const uint8_t *>(json.constData()),
                             size_t(json.size()));
    ac_session_close(h);
    if (s)
        rc_settings_close(s);
    if (!raw)
        return QJsonObject();
    QByteArray out = QByteArray::fromRawData(raw, int(strlen(raw)));
    out.detach();
    rc_string_free(raw);
    return QJsonDocument::fromJson(out).object();
}
#endif // ARKTOS_RUSTCORE

AutoSaver::AutoSaver(QObject *parent) : QObject(parent)
{
    Q_ASSERT(parent);
}

AutoSaver::~AutoSaver()
{
    if (m_timer.isActive()) {
        qWarning() << "AutoSaver: still active when destroyed, changes not saved.";
        if (parent() && parent()->metaObject())
            qWarning() << parent() << parent()->metaObject()->className() << "should call saveIfNeccessary";
    }
}

void AutoSaver::setSessionProfile(const QString &profileId)
{
    m_sessionProfile = profileId;
}

void AutoSaver::changeOccurred()
{
    changeOccurredFor(DirtyTabs);
}

void AutoSaver::changeOccurredFor(DirtyKind kind)
{
    int autosaveIn = AUTOSAVE_IN;
    int maxWait = MAXWAIT;
#if defined(ARKTOS_RUSTCORE)
    if (!m_sessionProfile.isEmpty()) {
        // MD08: the session domain's policy decides whether this
        // change class dirties the session at all and how the
        // debounce is timed — one resolved set per change.  An empty
        // verdict keeps the compiled-in constants (every class dirty,
        // 3s/15s timing) so a failure can never silently skip a save.
        static const char *const kinds[] = {
            "tabs", "chrome", "sidebar", "lifecycle" };
        const QJsonObject policy = sessionPolicyCall(
            m_sessionProfile,
            QJsonObject{
                {QLatin1String("op"), QLatin1String("policy")}});
        if (!policy.isEmpty()) {
            const int k = (int(kind) >= 0 && int(kind) < 4) ? int(kind) : 0;
            const QJsonObject dirtyOn =
                policy.value(QLatin1String("dirty_on")).toObject();
            if (!dirtyOn.value(QLatin1String(kinds[k])).toBool(true))
                return;
            if (policy.value(QLatin1String("interval_ms")).isDouble())
                autosaveIn =
                    policy.value(QLatin1String("interval_ms")).toInt();
            if (policy.value(QLatin1String("maxwait_ms")).isDouble())
                maxWait =
                    policy.value(QLatin1String("maxwait_ms")).toInt();
        }
    }
#else
    Q_UNUSED(kind);
#endif

    if (!m_firstChange.isValid())
        m_firstChange.start();

    if (m_firstChange.elapsed() > maxWait) {
        saveIfNeccessary();
    } else {
        m_timer.start(autosaveIn, this);
    }
}

void AutoSaver::timerEvent(QTimerEvent *event)
{
    if (event->timerId() == m_timer.timerId()) {
        saveIfNeccessary();
    } else {
        QObject::timerEvent(event);
    }
}

void AutoSaver::saveIfNeccessary()
{
    if (!m_timer.isActive())
        return;
    m_timer.stop();
    m_firstChange.invalidate();
    if (!QMetaObject::invokeMethod(parent(), "save", Qt::DirectConnection)) {
        qWarning() << "AutoSaver: error invoking slot save() on parent";
    }
}

