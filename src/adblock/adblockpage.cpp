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

#include "adblockpage.h"

#include "adblockmanager.h"
#include "adblocksubscription.h"
#include "adblockrule.h"

#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qwebenginepage.h>

#include <qdebug.h>

// #define ADBLOCKPAGE_DEBUG

AdBlockPage::AdBlockPage(QObject *parent)
    : QObject(parent)
{
}

// Returns the element-hiding selector of a "domain##selector" filter
// when it applies to host, an empty string otherwise.  Domain matching
// keeps the WebKit semantics: a matching ~domain exclusion drops the
// rule, a non-matching ~domain counts towards matching.
QString AdBlockPage::cssSelectorForHost(const AdBlockRule *rule, const QString &host)
{
    if (!rule->isEnabled())
        return QString();

    QString filter = rule->filter();
    int offset = filter.indexOf(QLatin1String("##"));
    if (offset == -1)
        return QString();

    if (offset > 0) {
        QStringList domains = filter.left(offset).split(QLatin1Char(','));

        bool match = false;
        for (const QString &domain : domains) {
            if (domain.isEmpty())
                continue;
            bool reverse = domain.startsWith(QLatin1Char('~'));
            if (reverse) {
                QString xdomain = domain.mid(1);
                if (host.endsWith(xdomain))
                    return QString();
                match = true;
            }
            if (host.endsWith(domain))
                match = true;
        }
        if (!match)
            return QString();
    }

    return filter.mid(offset + 2);
}

void AdBlockPage::applyRulesToPage(QWebEnginePage *page)
{
    if (!page)
        return;
    AdBlockManager *manager = AdBlockManager::instance();
    if (!manager->isEnabled())
        return;

    const QString host = page->url().host();
    QStringList selectors;
    const QList<AdBlockSubscription*> subscriptions = manager->subscriptions();
    for (const AdBlockSubscription *subscription : subscriptions) {
        const QList<const AdBlockRule*> rules = subscription->pageRules();
        for (const AdBlockRule *rule : rules) {
            const QString selector = cssSelectorForHost(rule, host);
            if (!selector.isEmpty())
                selectors.append(selector);
        }
    }
    if (selectors.isEmpty())
        return;

#if defined(ADBLOCKPAGE_DEBUG)
    qDebug() << "AdBlockPage::" << __FUNCTION__ << "hiding" << selectors.count() << "selectors on" << host;
#endif
    // QWebElement DOM access is gone in Qt WebEngine; the filters are
    // injected as a <style> element through runJavaScript instead.
    // Unlike the old synchronous DOM walk, the stylesheet also hides
    // matching elements added to the document after injection.
    const QString css = selectors.join(QLatin1Char(','))
        + QLatin1String(" { display: none !important; }");
    // Serialize the CSS as a JSON literal so selector contents can
    // never break out of the JavaScript string.
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
