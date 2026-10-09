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

#include "sandboxpolicy.h"

#include <qdir.h>
#include <qfileinfo.h>

QStringList SandboxPolicy::homeRelativeDeniedPaths()
{
    return {
        // Credentials, key material and agent state.
        QStringLiteral(".ssh"),
        QStringLiteral(".gnupg"),
        QStringLiteral(".pki"),
        QStringLiteral(".password-store"),
        QStringLiteral(".netrc"),
        QStringLiteral(".git-credentials"),
        QStringLiteral(".local/share/keyrings"),
        // Cloud / cluster / CLI tooling config carrying tokens.
        QStringLiteral(".aws"),
        QStringLiteral(".azure"),
        QStringLiteral(".kube"),
        QStringLiteral(".docker"),
        QStringLiteral(".config/gh"),
        // Other browsers' profiles (cookie and credential stores).
        QStringLiteral(".mozilla"),
        QStringLiteral(".thunderbird"),
        QStringLiteral(".config/google-chrome"),
        QStringLiteral(".config/chromium"),
        QStringLiteral(".config/BraveSoftware"),
        QStringLiteral(".config/microsoft-edge"),
        QStringLiteral(".config/microsoft-edge-dev"),
        QStringLiteral(".config/vivaldi"),
        QStringLiteral(".config/opera"),
        QStringLiteral(".config/epiphany"),
        QStringLiteral(".local/share/epiphany"),
        QStringLiteral(".config/falkon"),
        QStringLiteral(".config/qutebrowser"),
        // macOS equivalents — inert on other platforms, picked up by
        // the Seatbelt backend.
        QStringLiteral("Library/Keychains"),
        QStringLiteral("Library/Application Support/Google/Chrome"),
        QStringLiteral("Library/Application Support/Chromium"),
        QStringLiteral("Library/Application Support/Firefox"),
        QStringLiteral("Library/Application Support/BraveSoftware"),
    };
}

QStringList SandboxPolicy::readOnlyRootCandidates()
{
    return {
        QStringLiteral("/etc"),
        QStringLiteral("/usr"),
        QStringLiteral("/boot"),
    };
}

SandboxPolicy SandboxPolicy::defaultPolicy()
{
    SandboxPolicy policy;
    policy.homeDir = QDir::homePath();
    for (const QString &relative : homeRelativeDeniedPaths()) {
        const QString path = policy.homeDir + QLatin1Char('/') + relative;
        const QFileInfo info(path); // follows symlinks — masks cover the
                                    // link path itself, never the target
        if (info.isDir())
            policy.deniedDirs.append(path);
        else if (info.exists() || info.isSymLink())
            policy.deniedFiles.append(path);
        // Missing paths are deliberately dropped: masking them would
        // make bwrap create the mount point on the real filesystem.
    }
    for (const QString &root : readOnlyRootCandidates()) {
        if (QFileInfo(root).isDir())
            policy.readOnlyRoots.append(root);
    }
    return policy;
}
