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

#ifndef HOSTMATCH_H
#define HOSTMATCH_H

#include <qstring.h>
#include <qstringlist.h>
#include <qstringview.h>

// CA08: one home for the host-normalization + suffix-rule matching
// the per-site stores (ScriptControlManager, PopupBlocker), the
// cookie gate and the navigation policy each carried as private
// copies.  Two rule grammars live here and they are NOT the same:
//
//   matchesRule   — entries are bare host suffixes (normalize them
//                   through normalize() first): "example.com" covers
//                   itself and every "www.example.com" under it.
//   isOnDomainList — entries may additionally lead with '.': ".com"
//                   matches every com host AND com itself; the bare
//                   spelling keeps the matchesRule behavior.
namespace HostMatch {

// Lowercases the host and strips root/leading dots — a leading dot
// is meaningless in a suffix-matched store (the match already covers
// "example.com or any subdomain").
inline QString normalize(const QString &host)
{
    QString normalized = host.toLower();
    while (normalized.endsWith(QLatin1Char('.')))
        normalized.chop(1);
    while (normalized.startsWith(QLatin1Char('.')))
        normalized = normalized.mid(1);
    return normalized;
}

// CookieJar-style suffix matching: "example.com" governs
// "example.com" itself and every "www.example.com" under it.
inline bool matchesRule(const QStringList &rules, const QString &host)
{
    for (const QString &rule : rules) {
        if (host == rule || host.endsWith(QLatin1Char('.') + rule))
            return true;
    }
    return false;
}

// The cookie exception grammar: a rule matches the domain exactly or
// the domain ends with ".rule"; a leading-dot rule additionally
// matches the bare suffix itself (".com" covers "com").
inline bool isOnDomainList(const QStringList &rules, const QString &domain)
{
    for (const QString &rule : rules) {
        if (rule.startsWith(QLatin1String("."))) {
            if (domain.endsWith(rule))
                return true;
            const QStringView withoutDot =
                QStringView(rule).right(rule.size() - 1);
            if (domain == withoutDot)
                return true;
        } else {
            if (domain.endsWith(QLatin1Char('.') + rule))
                return true;
            if (rule == domain)
                return true;
        }
    }
    return false;
}

} // namespace HostMatch

#endif // HOSTMATCH_H
