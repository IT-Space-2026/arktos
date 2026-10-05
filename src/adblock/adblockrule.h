/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef ADBLOCKRULE_H
#define ADBLOCKRULE_H

#include <qregularexpression.h>
#include <qstringlist.h>

class QUrl;

/*
    One parsed AdBlock-Plus / uBlock-Origin filter line.

    Modern syntax handled here:

      Network filters:  pattern[$opt,opt=value,...]
        - type options (script, image, stylesheet, object, media, font,
          subdocument, document, xmlhttprequest, ping, websocket, other,
          and the legacy names background/object-subrequest/xbl/dtd)
          matched against QWebEngineUrlRequestInfo::resourceType()
        - third-party / first-party (~third-party) matched against the
          first-party (document) host
        - domain=d1|~d2 restricting the source document host
        - denyallow=d1|d2 excluding request target hosts (uBO)
        - important (uBO: block beats plain @@ exceptions)
        - redirect / redirect-rule to a bundled stub resource
        - removeparam[=name|all|/re/] query-parameter stripping (uBO)
        - badfilter disabling the identical filter in the same list
        - match-case
        Options that change semantics and cannot be implemented (csp,
        rewrite=, header=, replace=, cookie=, sitekey=, webrtc, popup,
        mp4) mark the rule unsupported so it never matches; unknown
        options are ignored.

      Cosmetic filters: domains ##sel / #@#sel / #?#sel / #@?#sel /
        #$#snippet / #@$#snippet / ##+js(scriptlet,args) and the uBO
        $$html marker (recognized but unsupported).
*/
class AdBlockRule
{

public:
    AdBlockRule(const QString &filter = QString());

    QString filter() const;
    void setFilter(const QString &filter);

    // True for every cosmetic-family rule (##, #@#, #?#, #@?#, #$#,
    // #@$#, +js(...) bodies, $$ HTML filters).  Historical name kept.
    bool isCSSRule() const { return m_cosmetic; }
    bool isCosmeticException() const { return m_cosmeticException; }
    // Body starts with +js( or the rule used #$# / #@$# (snippet).
    bool isScriptletRule() const { return m_scriptlet; }
    // Comma-separated domain list before the marker ("" = generic).
    QString cosmeticDomains() const;
    // Selector / snippet text after the marker.
    QString cosmeticBody() const;

    // resourceType is a QWebEngineUrlRequestInfo::ResourceType value
    // (-1 = unknown, e.g. autotests); documentHost is the first-party
    // host ("" = treat the request itself as first-party).
    bool networkMatch(const QString &encodedUrl,
                      const QString &documentHost = QString(),
                      int resourceType = -1) const;

    bool isException() const;
    void setException(bool exception);

    bool isEnabled() const;
    void setEnabled(bool enabled);
    // False when the filter uses an option we cannot honor; the rule
    // is inert but still shown in the UI.
    bool isSupported() const { return m_supported; }

    bool isImportant() const { return m_important; }
    bool isBadFilter() const { return m_badFilter; }
    // Canonical filter text a $badfilter twin would suppress.
    QString badFilterKey() const;

    QString redirectResource() const { return m_redirect; }
    // "" = not a removeparam rule; "*" = strip all params; otherwise a
    // parameter name or a /regular expression/.
    QString removeParam() const { return m_removeParam; }

    bool hasDomainOption() const { return !m_domainOption.isEmpty(); }

    // Document-level exception modifiers (uBO/ABP unbreak options).
    bool isDocumentException() const { return m_documentException; }
    bool isElemHide() const { return m_elemHide; }
    bool isGenericHide() const { return m_genericHide; }
    bool isGenericBlock() const { return m_genericBlock; }

    QString regExpPattern() const;
    void setPattern(const QString &pattern, bool isRegExp);

private:
    void parseOptions(const QStringList &options);

    QString m_filter;

    bool m_cosmetic;
    bool m_cosmeticException;
    bool m_scriptlet;
    int m_markerOffset;
    int m_markerLength;

    bool m_exception;
    bool m_enabled;
    bool m_supported;
    bool m_important;
    bool m_badFilter;
    bool m_documentException;
    bool m_elemHide;
    bool m_genericHide;
    bool m_genericBlock;

    enum PartyConstraint { AnyParty, ThirdParty, FirstParty };
    PartyConstraint m_party;

    quint32 m_typeMask;     // positive type options (0 = any)
    quint32 m_notTypeMask;  // ~type options
    QStringList m_domainOption;
    QStringList m_denyAllow;
    QString m_redirect;
    QString m_removeParam;

    QRegularExpression m_regExp;
};

#endif // ADBLOCKRULE_H
