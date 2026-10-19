/*
 * Copyright 2026 The Arktos Authors
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

#ifndef WEBKITBACKEND_H
#define WEBKITBACKEND_H

// WK06 — the WPE WebKit backend: build-gate skeleton only.
//
// The class is in the build whenever engine.pri resolves the webkit
// CONFIG — mirroring servobackend's compile-but-inert contract rather
// than conditional source exclusion: a `qmake webkit=1` build on a
// host whose pkg-config cannot resolve wpe-webkit-2.0 +
// wpe-platform-2.0 still compiles this TU, and the backend simply
// reports the engine unavailable (librariesLinked() == false,
// initialize() returns false, pageUnavailableReason() explains) — the
// no-libs honesty half of the gate.
//
// ARKTOS_WEBKIT_LINKED marks the builds where the packages did
// resolve: the TU then carries real libWPEWebKit references (the
// version probe below keeps the DT_NEEDED honest) and every binary
// linking it carries the ~/wpe sideloaded-glibc recipe from
// .devin/webkit-env.sh — sid ld.so interp + transitive DT_RPATH +
// the g_sort_array compat shim.
//
// WK07 grows this into the real backend: the EngineRegistry::addProbe
// registration (deliberately absent here — nothing may offer an
// engine that cannot host a page yet), the conservative capabilities
// table from .devin/WEBKIT-SPIKE.md, and the WebkitProfile/
// WebkitPage/WebkitView skeletons.

#include "engineinterface.h"

class WebkitBackend : public Engine::Backend
{
    Q_OBJECT

public:
    explicit WebkitBackend(QObject *parent = nullptr);

    // Configure-time pkg-config resolution — false on a webkit=1
    // build where the WPE packages were absent (the inert half of
    // compile-but-inert).
    static bool librariesLinked();
    // "x.y.z" from webkit_get_*_version — empty when not linked.
    static QString wpeVersion();

    QString id() const override;
    QString displayName() const override;
    QString engineNote() const override;
    Engine::Capabilities capabilities() const override;
    bool initialize() override;

    Engine::Profile *createProfile(const Engine::ProfileOptions &options,
                                   QObject *parent = nullptr) override;
    Engine::Page *createPage(Engine::Profile *profile,
                             QObject *parent = nullptr) override;
    QString pageUnavailableReason() const override;
    QWidget *createView(Engine::Page *page, QWidget *parent = nullptr) override;
};

#endif // WEBKITBACKEND_H
