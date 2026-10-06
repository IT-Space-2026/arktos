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

// libFuzzer target for the AdBlock-Plus / uBlock-Origin filter-line
// parser.  Input is split into lines the way
// AdBlockSubscription::loadRules() feeds the file to the parser; each
// line becomes an AdBlockRule and every accessor plus networkMatch()
// is exercised.

#include "adblockrule.h"

#include <qstring.h>
#include <qstringlist.h>

#include <stddef.h>
#include <stdint.h>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QString input =
            QString::fromUtf8(reinterpret_cast<const char *>(data),
                              qsizetype(size));

    // The tail of the input doubles as the request url + first-party
    // host so matching (not only parsing) sees fuzzed strings.
    const qsizetype half = input.size() / 2;
    const QString tail = input.mid(half);
    const QString requestUrl =
            QLatin1String("http://example.com/") + tail.left(512);
    const QString documentHost = tail.left(64);

    const QStringList lines =
            input.left(half).split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    for (const QString &line : lines) {
        AdBlockRule rule(line.left(4096));
        rule.filter();
        rule.isCSSRule();
        rule.isCosmeticException();
        rule.isScriptletRule();
        rule.cosmeticDomains();
        rule.cosmeticBody();
        rule.isException();
        rule.isEnabled();
        rule.isSupported();
        rule.isImportant();
        rule.isBadFilter();
        rule.badFilterKey();
        rule.redirectResource();
        rule.removeParam();
        rule.hasDomainOption();
        rule.isDocumentException();
        rule.isElemHide();
        rule.isGenericHide();
        rule.isGenericBlock();
        rule.regExpPattern();
        rule.matchToken();
        // resourceType carries QWebEngineUrlRequestInfo::ResourceType
        // values; -1 and out-of-range values hit the unknown paths.
        for (int type = -1; type <= 24; ++type)
            rule.networkMatch(requestUrl, documentHost, type);
        // Exception + disabled twins reuse the same compiled pattern.
        rule.setException(!rule.isException());
        rule.setEnabled(false);
        rule.networkMatch(requestUrl, QString(), -1);
    }
    return 0;
}
