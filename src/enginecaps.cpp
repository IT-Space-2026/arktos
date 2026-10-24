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

#include "enginecaps.h"

#ifdef ARKTOS_RUSTCORE

#include <qhash.h>
#include <qjsondocument.h>
#include <qjsonobject.h>

#include <arktos_core.h>

// engine id -> its parsed "features" object.  Populated lazily —
// one FFI call per engine the chrome asks about (an engine switch
// changes the id being queried, not the data); the static table
// means the cache never invalidates.  Failed lookups cache an
// empty object too — "this id resolved to no row" is itself
// static truth, not a transient error worth retrying per paint.
static QHash<QString, QJsonObject> s_featuresByEngine;

static const QJsonObject &featuresFor(const QString &engineId)
{
    auto it = s_featuresByEngine.constFind(engineId);
    if (it != s_featuresByEngine.constEnd())
        return it.value();

    QJsonObject features;
    AcBuffer out{};
    const QByteArray id = engineId.toUtf8();
    if (ac_engine_caps(id.constData(), &out) == AC_OK
            && out.data && out.len) {
        const QJsonDocument doc = QJsonDocument::fromJson(
            QByteArray::fromRawData(
                reinterpret_cast<const char *>(out.data),
                int(out.len)));
        features = doc.object()
            .value(QLatin1String("features")).toObject();
    }
    ac_buffer_free(out);
    return s_featuresByEngine.insert(engineId, features).value();
}

EngineCaps::State EngineCaps::featureState(const QString &engineId,
                                           const QString &feature)
{
    const QString state = featuresFor(engineId)
        .value(feature).toObject()
        .value(QLatin1String("state")).toString();
    if (state == QLatin1String("supported"))
        return State::Supported;
    if (state == QLatin1String("partial"))
        return State::Partial;
    if (state == QLatin1String("unsupported"))
        return State::Unsupported;
    return State::Unknown;
}

QString EngineCaps::featureNote(const QString &engineId,
                                const QString &feature)
{
    return featuresFor(engineId)
        .value(feature).toObject()
        .value(QLatin1String("note")).toString();
}

#else // !ARKTOS_RUSTCORE — the registry lives in the crate; without
      // it every lookup is Unknown and callers keep their adapter-
      // local verdicts (identical to the pre-MS05 behavior).

EngineCaps::State EngineCaps::featureState(const QString &,
                                           const QString &)
{
    return State::Unknown;
}

QString EngineCaps::featureNote(const QString &, const QString &)
{
    return QString();
}

#endif // ARKTOS_RUSTCORE

bool EngineCaps::featureUsable(const QString &engineId,
                               const QString &feature, bool fallback)
{
    const State state = featureState(engineId, feature);
    if (state == State::Unknown)
        return fallback;
    return state != State::Unsupported;
}
