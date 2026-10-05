/**
 * Copyright (c) 2009, Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright (c) 2026, The Arora Authors
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

#include "adblockpage.h"

#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockrule.h"

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qwebenginepage.h>

#include <qdebug.h>

// #define ADBLOCKPAGE_DEBUG

AdBlockPage::AdBlockPage(QObject *parent)
    : QObject(parent)
{
}

// True when the comma-separated domain list before a cosmetic marker
// applies to host.  "~domain" entries are exclusions; ABP semantics:
// an excluded match drops the rule, otherwise any positive match (or a
// list of only negatives) applies.
static bool cosmeticDomainsMatch(const QString &domains, const QString &host)
{
    if (domains.isEmpty())
        return true;

    bool match = false;
    bool hasPositive = false;
    for (const QString &domain : domains.split(QLatin1Char(','))) {
        if (domain.isEmpty())
            continue;
        if (domain.startsWith(QLatin1Char('~'))) {
            const QString excluded = domain.mid(1);
            if (host == excluded || host.endsWith(QLatin1Char('.') + excluded))
                return false;
        } else {
            hasPositive = true;
            if (host == domain || host.endsWith(QLatin1Char('.') + domain))
                match = true;
        }
    }
    return !hasPositive || match;
}

// Rewrites the ABP extended-selector pseudo-classes that have a
// Chromium-native equivalent (":-abp-has(x)" -> ":has(x)",
// ":-abp-has-not(x)" -> ":not(:has(x))").  Parens are matched so the
// rewrite is balanced.
static QString translateAbpPseudos(QString body)
{
    static const struct {
        const char *token;
        const char *replacement;
        bool addParen;
    } pseudos[] = {
        { ":-abp-has-not(", ":not(:has(", true },
        { ":-abp-has(", ":has(", false },
        { ":-abp-if(", ":has(", false },
        { ":-abp-if-not(", ":not(:has(", true },
    };
    for (const auto &pseudo : pseudos) {
        int offset = 0;
        const QString token = QLatin1String(pseudo.token);
        while ((offset = body.indexOf(token, offset)) != -1) {
            const int argsStart = offset + token.size();
            // find the matching close paren
            int depth = 1;
            int close = argsStart;
            for (; close < body.size(); ++close) {
                if (body.at(close) == QLatin1Char('('))
                    ++depth;
                else if (body.at(close) == QLatin1Char(')') && --depth == 0)
                    break;
            }
            if (depth != 0)
                break; // unbalanced — leave as-is
            body.replace(offset, argsStart - offset + (close - argsStart) + 1,
                         QLatin1String(pseudo.replacement)
                         + body.mid(argsStart, close - argsStart)
                         + QLatin1String(")")
                         + (pseudo.addParen ? QLatin1String(")") : QString()));
            offset = argsStart;
        }
    }
    return body;
}

// Procedural/extended operators that need JavaScript (not expressible
// as plain CSS).  Returns the split into a base selector + ordered op
// list, or false when body is not procedural.
struct CosmeticOp {
    QString name;
    QString arg;
};

static bool splitProceduralOps(const QString &body, QString *base,
                               QList<CosmeticOp> *ops)
{
    static const QStringList operatorNames = {
        QStringLiteral("has-text"),      // uBO + ABP :-abp-contains
        QStringLiteral("contains"),
        QStringLiteral("matches-css"),   // uBO + ABP :-abp-properties
        QStringLiteral("style"),
        QStringLiteral("remove"),
        QStringLiteral("remove-attr"),
        QStringLiteral("remove-class"),
        QStringLiteral("upward"),
        QStringLiteral("min-text-length"),
        QStringLiteral("xpath"),         // recognized, unsupported
    };
    // ABP spellings normalized to the uBO op names.
    static const QHash<QString, QString> aliases = {
        { QStringLiteral("-abp-contains"), QStringLiteral("has-text") },
        { QStringLiteral("contains"), QStringLiteral("has-text") },
        { QStringLiteral("-abp-properties"), QStringLiteral("matches-css") },
    };

    QString rest = body;
    QList<CosmeticOp> reversed;
    while (rest.endsWith(QLatin1Char(')'))) {
        // Find the innermost trailing ":op(" — the last operator token
        // whose parens balance to the end of the string.
        int foundPos = -1;
        QString foundName;
        for (const QString &name : operatorNames + aliases.keys()) {
            const QString token = QLatin1Char(':') + name + QLatin1Char('(');
            const int pos = rest.lastIndexOf(token);
            if (pos == -1 || pos <= foundPos)
                continue;
            int depth = 0;
            for (int i = pos + 1; i < rest.size(); ++i) {
                if (rest.at(i) == QLatin1Char('('))
                    ++depth;
                else if (rest.at(i) == QLatin1Char(')'))
                    --depth;
            }
            if (depth == 0) {
                foundPos = pos;
                foundName = name;
            }
        }
        if (foundPos == -1)
            break;
        const int argStart = foundPos + 1 + foundName.size() + 1;
        CosmeticOp op;
        op.name = aliases.value(foundName, foundName);
        op.arg = rest.mid(argStart, rest.size() - argStart - 1);
        reversed.append(op);
        rest = rest.left(foundPos);
    }

    if (reversed.isEmpty())
        return false;

    *base = rest;
    std::reverse(reversed.begin(), reversed.end());
    *ops = reversed;
    return true;
}

// Parses "+js(name, arg, 'quoted arg')" or "#$# snippet args" bodies
// into { name, args[] }.
static QJsonObject parseScriptlet(const QString &body)
{
    QJsonObject object;
    QString text = body;
    QStringList parts;

    if (text.startsWith(QLatin1String("+js(")) && text.endsWith(QLatin1Char(')')))
        text = text.mid(4, text.size() - 5);

    // Comma- or whitespace-separated arguments; 'single' and "double"
    // quotes group.
    QString current;
    QChar quote;
    for (const QChar c : text) {
        if (!quote.isNull()) {
            if (c == quote)
                quote = QChar();
            else
                current += c;
            continue;
        }
        if (c == QLatin1Char('\'') || c == QLatin1Char('"')) {
            quote = c;
        } else if (c == QLatin1Char(',') || c == QLatin1Char(' ')) {
            if (!current.isEmpty()) {
                parts.append(current);
                current.clear();
            }
        } else {
            current += c;
        }
    }
    if (!current.isEmpty())
        parts.append(current);

    if (parts.isEmpty())
        return object;
    object.insert(QLatin1String("n"), parts.takeFirst());
    object.insert(QLatin1String("a"), QJsonArray::fromStringList(parts));
    return object;
}

// Shared JavaScript implementing the procedural operators and the
// small scriptlet set.  Payloads arrive as JSON literals.
static QString cosmeticEngineScript(const QJsonArray &procedures,
                                    const QJsonArray &scriptlets)
{
    const QByteArray json = QJsonDocument(procedures).toJson(QJsonDocument::Compact);
    const QByteArray jsonScriptlets =
        QJsonDocument(scriptlets).toJson(QJsonDocument::Compact);

    QString script = QString::fromUtf8(
        "(function(){"
        "var PROCS=%1,SCRS=%2;"
        "function hide(e){e.style.setProperty('display','none','important');}"
        "function qsa(s){try{return Array.prototype.slice.call("
        "document.querySelectorAll(s));}catch(e){return[];}}"
        "function matchText(e,a){"
        "if(a.charAt(0)==='/'&&a.charAt(a.length-1)==='/'&&a.length>1){"
        "try{return new RegExp(a.slice(1,-1)).test(e.textContent);}"
        "catch(x){return false;}}"
        "return e.textContent.indexOf(a)!==-1;}"
        "function walkPath(p,create){"
        "var segs=p.split('.'),o=window;"
        "for(var i=0;i<segs.length-1;i++){"
        "if(!(segs[i] in o)){if(!create)return[null,null];"
        "o[segs[i]]={};}o=o[segs[i]];if(o===null||typeof o!=='object'&&"
        "typeof o!=='function')return[null,null];}"
        "return[o,segs[segs.length-1]];}"
        "function constValue(v){"
        "switch(v){case'true':return true;case'false':return false;"
        "case'null':return null;case'undefined':return undefined;"
        "case'noopFunc':case'noopCallback':return function(){};"
        "case'emptyObj':return{};case'emptyArr':return[];"
        "case'emptyStr':return'';}"
        "var n=parseFloat(v);return isNaN(n)?v:n;}"
        "var scriptlets={"
        "'set-constant':function(a){var t=walkPath(a[0],true);"
        "if(!t[0])return;var v=constValue(a[1]||'undefined');"
        "try{Object.defineProperty(t[0],t[1],{get:function(){return v;},"
        "set:function(){},configurable:true});}catch(e){}},"
        "'abort-on-property-read':function(a){var t=walkPath(a[0],true);"
        "if(!t[0])return;try{Object.defineProperty(t[0],t[1],{"
        "get:function(){throw new ReferenceError(a[0]+' is not defined');},"
        "configurable:true});}catch(e){}},"
        "'abort-on-property-write':function(a){var t=walkPath(a[0],true);"
        "if(!t[0])return;try{Object.defineProperty(t[0],t[1],{"
        "set:function(){throw new ReferenceError(a[0]+' is not defined');},"
        "configurable:true});}catch(e){}},"
        "'hide-if-contains':function(a){var sel=a[1]||'*';"
        "qsa(sel).forEach(function(e){if(matchText(e,a[0]||''))hide(e);});},"
        "'remove-class':function(a){qsa(a[1]||'*').forEach(function(e){"
        "e.classList.remove(a[0]);});}"
        "};"
        "scriptlets['aopr']=scriptlets['abort-on-property-read'];"
        "scriptlets['aopw']=scriptlets['abort-on-property-write'];"
        "scriptlets['set']=scriptlets['set-constant'];"
        "PROCS.forEach(function(p){"
        "var els=qsa(p.s),acted=false;"
        "p.ops.forEach(function(op){var name=op[0],a=op[1]||'';"
        "if(name==='has-text'){els=els.filter(function(e){return matchText(e,a);});}"
        "else if(name==='matches-css'){var sep=a.indexOf(':');"
        "if(sep!==-1){var prop=a.slice(0,sep).trim(),val=a.slice(sep+1).trim();"
        "els=els.filter(function(e){var got=getComputedStyle(e)"
        ".getPropertyValue(prop);if(!got)return false;"
        "if(val.charAt(0)==='/'&&val.charAt(val.length-1)==='/'){"
        "try{return new RegExp(val.slice(1,-1)).test(got);}catch(x){}}"
        "return got.indexOf(val)!==-1;});}}"
        "else if(name==='min-text-length'){var n=parseInt(a)||0;"
        "els=els.filter(function(e){return e.textContent.trim().length>=n;});}"
        "else if(name==='upward'){var n2=parseInt(a);"
        "els=els.map(function(e){if(!isNaN(n2)){var o=e;"
        "for(var i=0;i<n2&&o;i++)o=o.parentElement;return o;}"
        "try{return e.closest(a);}catch(x){return null;}})"
        ".filter(function(e){return e;});}"
        "else if(name==='style'){acted=true;els.forEach(function(e){"
        "e.style.cssText+=';'+a;});}"
        "else if(name==='remove'){acted=true;els.forEach(function(e){"
        "e.remove();});}"
        "else if(name==='remove-attr'){acted=true;els.forEach(function(e){"
        "e.removeAttribute(a);});}"
        "else if(name==='remove-class'){acted=true;els.forEach(function(e){"
        "a.split(/\\s+/).forEach(function(c){if(c)e.classList.remove(c);});});}"
        "});"
        "if(!acted)els.forEach(hide);"
        "});"
        "SCRS.forEach(function(s){var fn=scriptlets[s.n];"
        "if(!fn){if(s.n.slice(-3)==='.js')fn=scriptlets[s.n.slice(0,-3)];}"
        "if(fn)try{fn(s.a||[]);}catch(e){}});"
        "})()")
        .arg(QLatin1String(json), QLatin1String(jsonScriptlets));
    return script;
}

void AdBlockPage::applyRulesToPage(QWebEnginePage *page)
{
    if (!page)
        return;
    AdBlockManager *manager = AdBlockManager::instance();
    if (!manager->isEnabled())
        return;

    const QString host = page->url().host();
    const QString documentUrl = QString::fromUtf8(page->url().toEncoded());
    const QList<AdBlockSubscription*> subscriptions = manager->subscriptions();

    // Document-level cosmetic exceptions: @@||site^$elemhide disables
    // element hiding on the page entirely, @@||site^$generichide only
    // suppresses the generic (domain-less) rules.
    bool elemHide = false;
    bool genericHide = false;
    for (const AdBlockSubscription *subscription : subscriptions) {
        if (!subscription->isEnabled())
            continue;
        for (const AdBlockRule *rule : subscription->networkExceptionRules()) {
            if (rule->isElemHide()
                && rule->networkMatch(documentUrl, host, 0))
                elemHide = true;
            else if (rule->isGenericHide()
                     && rule->networkMatch(documentUrl, host, 0))
                genericHide = true;
        }
    }
    if (elemHide)
        return;

    QStringList cssBodies;
    QStringList exceptions;
    QJsonArray procedures;
    QJsonArray scriptlets;

    for (const AdBlockSubscription *subscription : subscriptions) {
        if (!subscription->isEnabled())
            continue;
        const QList<const AdBlockRule*> rules = subscription->pageRules();
        for (const AdBlockRule *rule : rules) {
            if (!rule->isEnabled())
                continue;
            const QString domains = rule->cosmeticDomains();
            if (!cosmeticDomainsMatch(domains, host))
                continue;
            if (genericHide && domains.isEmpty()
                && !rule->isCosmeticException())
                continue;
            const QString body = rule->cosmeticBody();
            if (body.isEmpty())
                continue;

            if (rule->isCosmeticException()) {
                exceptions.append(body);
                continue;
            }
            if (exceptions.contains(body))
                continue;

            // ABP #$# bodies can be "sel { declarations }" CSS
            // injections rather than snippet calls.
            const int brace = body.indexOf(QLatin1Char('{'));
            if (rule->isScriptletRule() && brace != -1
                && body.endsWith(QLatin1Char('}'))) {
                QJsonObject procedure;
                procedure.insert(QLatin1String("s"),
                                 body.left(brace).trimmed());
                procedure.insert(QLatin1String("o"), body);
                procedure.insert(QLatin1String("ops"),
                                 QJsonArray() << (QJsonArray()
                                     << QLatin1String("style")
                                     << body.mid(brace + 1,
                                                 body.size() - brace - 2)));
                procedures.append(procedure);
                continue;
            }

            if (rule->isScriptletRule()) {
                QJsonObject scriptlet = parseScriptlet(body);
                if (!scriptlet.isEmpty()) {
                    scriptlet.insert(QLatin1String("o"), body);
                    scriptlets.append(scriptlet);
                }
                continue;
            }

            QString base;
            QList<CosmeticOp> ops;
            if (splitProceduralOps(body, &base, &ops)) {
                QJsonArray opArray;
                bool supported = true;
                for (const CosmeticOp &op : ops) {
                    if (op.name == QLatin1String("xpath")) {
                        supported = false;
                        break;
                    }
                    opArray.append(QJsonArray() << op.name << op.arg);
                }
                if (!supported)
                    continue;
                QJsonObject procedure;
                procedure.insert(QLatin1String("s"), base);
                procedure.insert(QLatin1String("o"), body);
                procedure.insert(QLatin1String("ops"), opArray);
                procedures.append(procedure);
                continue;
            }

            cssBodies.append(body);
        }
    }

    // Cosmetic exceptions remove collected rules with the same body.
    for (const QString &exception : exceptions) {
        cssBodies.removeAll(exception);
        for (int i = procedures.size() - 1; i >= 0; --i) {
            if (procedures.at(i).toObject().value(QLatin1String("o"))
                    .toString() == exception)
                procedures.removeAt(i);
        }
        for (int i = scriptlets.size() - 1; i >= 0; --i) {
            if (scriptlets.at(i).toObject().value(QLatin1String("o"))
                    .toString() == exception)
                scriptlets.removeAt(i);
        }
    }

    QStringList cssSelectors;
    for (const QString &body : cssBodies) {
        const QString css = translateAbpPseudos(body);
        if (css.contains(QLatin1String(":-abp-"))
            || css.contains(QLatin1String("+js(")))
            continue; // pseudo we cannot translate
        cssSelectors.append(css);
    }

    if (!cssSelectors.isEmpty()) {
        // QWebElement DOM access is gone in Qt WebEngine; the filters
        // are injected as a <style> element through runJavaScript.
        // Wrapping the joined selectors in :is() gives a forgiving
        // selector list so one malformed rule can't kill the rest.
        const QString css = QLatin1String(":is(")
            + cssSelectors.join(QLatin1Char(','))
            + QLatin1String(") { display: none !important; }");
        const QByteArray jsonCss = QJsonDocument(QJsonArray() << css)
            .toJson(QJsonDocument::Compact);
        const QString script = QLatin1String(
            "(function(){var s=document.getElementById('arora-adblock');"
            "if(!s){s=document.createElement('style');s.id='arora-adblock';"
            "document.documentElement.appendChild(s);}"
            "s.textContent=%1[0];})()")
            .arg(QString::fromUtf8(jsonCss));
        page->runJavaScript(script);
    }

    if (!procedures.isEmpty() || !scriptlets.isEmpty())
        page->runJavaScript(cosmeticEngineScript(procedures, scriptlets));
}
