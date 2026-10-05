/*
 * Copyright 2009 Jonas Gehring <jonas.gehring@boolsoft.org>
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

#include "schemeaccesshandler.h"

#include "fileaccesshandler.h"

#include <qwebengineprofile.h>
#include <qwebengineurlscheme.h>

SchemeAccessHandler::SchemeAccessHandler(QObject *parent)
    : QWebEngineUrlSchemeHandler(parent)
{
}

void SchemeAccessHandler::registerUrlSchemes()
{
    // file:// is a built-in scheme and cannot take a custom handler, so
    // directory listings are served on arora-file:// instead (WebPage
    // redirects file:// directory navigations there).
    QWebEngineUrlScheme scheme(FileAccessHandler::schemeName());
    scheme.setSyntax(QWebEngineUrlScheme::Syntax::Path);
    scheme.setFlags(QWebEngineUrlScheme::SecureScheme
                    | QWebEngineUrlScheme::LocalScheme
                    | QWebEngineUrlScheme::LocalAccessAllowed
                    | QWebEngineUrlScheme::ViewSourceAllowed
                    | QWebEngineUrlScheme::CorsEnabled);
    QWebEngineUrlScheme::registerScheme(scheme);
}

void SchemeAccessHandler::installAll(QWebEngineProfile *profile, QObject *parent)
{
    FileAccessHandler *fileHandler = new FileAccessHandler(parent);
    profile->installUrlSchemeHandler(fileHandler->scheme(), fileHandler);
}
