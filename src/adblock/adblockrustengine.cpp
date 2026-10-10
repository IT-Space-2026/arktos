/**
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

#include "adblockrustengine.h"

#include "adblockresourcehandler.h"

#include <qjsondocument.h>

// C API implemented by src/adblock/rust (arora-adblock-ffi staticlib).
extern "C" {

struct AroraCheckResult {
    int action;             // 0 = allow, 1 = block, 2 = redirect
    char *redirect_url;     // data: URL or resource name
    char *rewritten_url;    // URL with $removeparam params stripped
};

AroraAdBlockEngine *arora_adblock_engine_new(const char *rules,
                                           size_t rules_len);
int arora_adblock_engine_add_resource(AroraAdBlockEngine *engine,
                                      const char *name, const char *mime,
                                      const unsigned char *content,
                                      size_t content_len);
AroraCheckResult arora_adblock_engine_check(const AroraAdBlockEngine *engine,
                                            const char *url,
                                            const char *source_url,
                                            const char *request_type);
char *arora_adblock_engine_cosmetic(const AroraAdBlockEngine *engine,
                                    const char *url);
void arora_adblock_engine_free(AroraAdBlockEngine *engine);
void arora_adblock_string_free(char *s);
}

// Maps QWebEngineUrlRequestInfo::ResourceType values (numeric, no
// QtWebEngine include needed) to the webRequest spellings adblock-rust
// parses; anything unmapped becomes "other".
static QByteArray requestTypeName(int resourceType)
{
    switch (resourceType) {
    case 0: case 19: return QByteArrayLiteral("main_frame");
    case 1: case 20: return QByteArrayLiteral("sub_frame");
    case 2:  return QByteArrayLiteral("stylesheet");
    case 3:  return QByteArrayLiteral("script");
    case 4: case 12: return QByteArrayLiteral("image");
    case 5:  return QByteArrayLiteral("font");
    case 7: case 17: return QByteArrayLiteral("object");
    case 8:  return QByteArrayLiteral("media");
    case 13: return QByteArrayLiteral("xmlhttprequest");
    case 14: return QByteArrayLiteral("ping");
    case 254: return QByteArrayLiteral("websocket");
    default: return QByteArrayLiteral("other");
    }
}

AdBlockRustEngine::AdBlockRustEngine()
    : m_engine(nullptr)
{
}

AdBlockRustEngine::~AdBlockRustEngine()
{
    arora_adblock_engine_free(m_engine);
}

AdBlockRustEngine *AdBlockRustEngine::create(const QByteArray &ruleText)
{
    if (ruleText.isEmpty())
        return nullptr;

    AdBlockRustEngine *wrapper = new AdBlockRustEngine;
    wrapper->m_engine = arora_adblock_engine_new(
        ruleText.constData(), size_t(ruleText.size()));
    if (!wrapper->m_engine) {
        delete wrapper;
        return nullptr;
    }

    // Register every name the resource handler can serve (canonical
    // stubs plus the alias spellings filter lists use) so a matching
    // $redirect= rule resolves to a data: URL at check() time.
    for (const QByteArray &name : AdBlockResourceHandler::registrationNames()) {
        QByteArray mime, body;
        const QByteArray canonical = AdBlockResourceHandler::canonicalResourceName(
            QString::fromLatin1(name));
        if (!AdBlockResourceHandler::resourceFor(canonical, &mime, &body))
            continue;
        arora_adblock_engine_add_resource(
            wrapper->m_engine, name.constData(), mime.constData(),
            reinterpret_cast<const unsigned char *>(body.constData()),
            size_t(body.size()));
        // The crate resolves a registered stub to a data: URL of the
        // form data:<mime>;base64,<payload>; remember which canonical
        // resource produced it so check() can hand back the servable
        // name instead.
        wrapper->m_stubUrls.insert(
            "data:" + mime + ";base64," + body.toBase64(), canonical);
    }
    return wrapper;
}

AdBlockDecision AdBlockRustEngine::check(const QUrl &url,
                                         const QUrl &firstPartyUrl,
                                         int resourceType) const
{
    AdBlockDecision decision;
    QMutexLocker locker(&m_mutex);

    const QByteArray urlUtf8 = url.toEncoded();
    const QByteArray sourceUtf8 = firstPartyUrl.isEmpty()
        ? urlUtf8 : firstPartyUrl.toEncoded();
    const QByteArray type = requestTypeName(resourceType);
    const AroraCheckResult result = arora_adblock_engine_check(
        m_engine, urlUtf8.constData(), sourceUtf8.constData(),
        type.constData());

    if (result.action == 2) {
        const QString redirect = QString::fromUtf8(
            result.redirect_url ? result.redirect_url : "");
        const QByteArray stubName = m_stubUrls.value(redirect.toUtf8());
        if (redirect.startsWith(QLatin1String("data:"))
                && stubName.isEmpty()) {
            decision.action = AdBlockDecision::Redirect;
            decision.redirectUrl = redirect;
        } else {
            // A resolved bundled stub (or an unresolved resource name)
            // routes through the same canonical-name table the native
            // path uses — arora-resource: is servable where a data:
            // redirect is refused by Chromium.
            const QByteArray canonical = !stubName.isEmpty()
                ? stubName
                : AdBlockResourceHandler::canonicalResourceName(redirect);
            if (!canonical.isEmpty()) {
                decision.action = AdBlockDecision::Redirect;
                decision.redirectResource = QString::fromLatin1(canonical);
            } else {
                decision.action = AdBlockDecision::Block;
            }
        }
    } else if (result.action == 1) {
        decision.action = AdBlockDecision::Block;
    } else if (result.rewritten_url) {
        // $removeparam rewrite: load the stripped URL instead.
        const QString rewritten = QString::fromUtf8(result.rewritten_url);
        if (rewritten != QString::fromUtf8(urlUtf8)) {
            decision.action = AdBlockDecision::Redirect;
            decision.redirectUrl = rewritten;
        }
    }

    arora_adblock_string_free(result.redirect_url);
    arora_adblock_string_free(result.rewritten_url);
    return decision;
}

QJsonObject AdBlockRustEngine::cosmetic(const QUrl &documentUrl) const
{
    QMutexLocker locker(&m_mutex);
    const QByteArray urlUtf8 = documentUrl.toEncoded();
    char *json = arora_adblock_engine_cosmetic(m_engine, urlUtf8.constData());
    if (!json)
        return QJsonObject();
    const QByteArray bytes(json);
    arora_adblock_string_free(json);
    return QJsonDocument::fromJson(bytes).object();
}
