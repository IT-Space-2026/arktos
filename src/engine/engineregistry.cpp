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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "engineregistry.h"

#include "webenginebackend.h"

#include <qpainter.h>
#include <qpalette.h>
#include <qpixmap.h>
#include <qsettings.h>

namespace {

QList<Engine::Backend *> &registry()
{
    static QList<Engine::Backend *> backends;
    return backends;
}

void ensureWebEngine()
{
    if (registry().isEmpty())
        registry().append(WebEngineBackend::instance());
}

QList<void (*)()> &probes()
{
    static QList<void (*)()> list;
    return list;
}

} // namespace

void EngineRegistry::addProbe(void (*probe)())
{
    if (probe)
        probes().append(probe);
}

QList<Engine::Backend *> EngineRegistry::backends()
{
    ensureWebEngine();
    for (void (*probe)() : probes())
        probe();
    return registry();
}

Engine::Backend *EngineRegistry::backendForId(const QString &id)
{
    for (Engine::Backend *backend : backends()) {
        if (backend->id() == id)
            return backend;
    }
    return nullptr;
}

void EngineRegistry::registerBackend(Engine::Backend *backend)
{
    ensureWebEngine();
    if (backend && !registry().contains(backend))
        registry().append(backend);
}

void EngineRegistry::unregisterBackend(Engine::Backend *backend)
{
    registry().removeAll(backend);
}

QString EngineRegistry::defaultBackendId()
{
    QSettings settings;
    return settings.value(QLatin1String("MainWindow/defaultEngine"),
                          QStringLiteral("webengine")).toString();
}

void EngineRegistry::setDefaultBackendId(const QString &id)
{
    QSettings settings;
    settings.setValue(QLatin1String("MainWindow/defaultEngine"), id);
}

Engine::Backend *EngineRegistry::defaultBackend()
{
    if (Engine::Backend *backend = backendForId(defaultBackendId()))
        return backend;
    return backendForId(QStringLiteral("webengine"));
}

QIcon EngineRegistry::backendIcon(const QString &id, const QPalette &palette,
                                  int extent)
{
    QPixmap pixmap(extent, extent);
    pixmap.fill(Qt::transparent);
    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    QString label;
    if (id == QLatin1String("webengine"))
        label = QStringLiteral("C");
    else if (id == QLatin1String("servo"))
        label = QStringLiteral("S");
    else if (!id.isEmpty())
        label = id.left(1).toUpper();
    else
        label = QStringLiteral("?");

    painter.setPen(Qt::NoPen);
    painter.setBrush(palette.color(QPalette::Accent).isValid()
        ? palette.color(QPalette::Accent)
        : palette.color(QPalette::Highlight));
    painter.drawRoundedRect(pixmap.rect().adjusted(0, 0, -1, -1), 3, 3);

    QFont font = painter.font();
    font.setPixelSize(qMax(8, extent - 5));
    font.setBold(true);
    painter.setFont(font);
    painter.setPen(palette.color(QPalette::HighlightedText));
    painter.drawText(pixmap.rect(), Qt::AlignCenter, label);
    painter.end();
    return QIcon(pixmap);
}
