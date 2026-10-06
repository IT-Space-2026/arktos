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

#ifndef BROWSERPROFILE_H
#define BROWSERPROFILE_H

#include <qstring.h>

class QWebEngineProfile;

// Application-wide QWebEngineProfile accessors and the code that
// applies the QSettings "websettings"/"network" groups to a profile.
// They live here so ported modules can use them while
// browserapplication.cpp is still uncompiled — MIG15 delegates
// BrowserApplication::webEngineProfile()/loadSettings() to these.
namespace BrowserProfile {

// The persistent "normal" browsing profile.  QWebEngineProfile::
// defaultProfile() is itself off-the-record in Qt6 (MIG06), so Arora
// browses on the named profile "arora".  Lazily created, qApp-owned.
QWebEngineProfile *normalProfile();

// Lazily created off-the-record profile for private browsing (MIG03):
// nothing — cookies, cache, storage — persists to disk.
QWebEngineProfile *privateProfile();

// The private profile only if it has already been brought up —
// nullptr otherwise.  For callers that want to adjust an existing
// private session without forcing one into existence (settings
// re-application).
QWebEngineProfile *privateProfileIfCreated();

// The user agent Arora sends when no override is configured (UA01):
// Qt's factory UA minus the "QtWebEngine/<ver>" product token, which
// bot-detection fingerprints as automation (Google /sorry/ blocks).
QString defaultHttpUserAgent();

// Applies the persisted preferences to the profile's
// QWebEngineSettings (was BrowserApplication::loadSettings() writing
// to QWebSettings::globalSettings()):
//   websettings group  -> fonts, WebAttribute toggles, userStyleSheet
//                         (injected through QWebEngineScript —
//                         QWebSettings::setUserStyleSheetUrl is gone)
//   network group      -> setHttpCacheType/setHttpCacheMaximumSize
//                         and setHttpAcceptLanguage
// Settings with no WebEngine equivalent (DeveloperExtrasEnabled,
// ZoomTextOnly, maximumPagesInCache, the access-keys feature) are
// intentionally not applied; see MIG11 notes in .devin/Arora-Task.md.
void applySettings(QWebEngineProfile *profile);

} // namespace BrowserProfile

#endif // BROWSERPROFILE_H
