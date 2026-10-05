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

#ifndef ADBLOCKRUSTENGINE_H
#define ADBLOCKRUSTENGINE_H

#include "adblocknetwork.h"

#include <qjsonobject.h>
#include <qmutex.h>

class QUrl;
struct AroraAdBlockEngine;

/*
    Thin Qt wrapper over Brave's adblock-rust engine, reached through
    the C FFI static library in rust/.  Only compiled when qmake is
    invoked with CONFIG+=adblock_rust (DEFINES += ARORA_ADBLOCK_RUST);
    the native C++ matcher remains the default engine.

    Arora keeps its own subscription manager, UI and on-disk rule
    storage; this class only does the matching.  AdBlockNetwork
    serializes the enabled subscriptions' rules into ABP/uBO list
    text and hands them to adblock::Engine, which natively applies
    semantics the native matcher only approximates (full PSL-aware
    party detection, $badfilter, $denyallow, $removeparam, redirect
    resource resolution, scriptlet argument substitution).

    check()/cosmetic() may be called from the WebEngine IO thread;
    calls are serialized internally because the Sync guarantees of
    the wrapped Engine are an implementation detail we do not rely on.
*/
class AdBlockRustEngine
{
public:
    ~AdBlockRustEngine();

    // Builds an engine from serialized filter text and registers the
    // bundled stub resources so $redirect= resolves to data: URLs.
    // Slow for large lists — call outside locks.  Returns 0 when the
    // text is empty or the engine cannot be built.
    static AdBlockRustEngine *create(const QByteArray &ruleText);

    bool isValid() const { return m_engine != 0; }

    // Same contract as AdBlockNetwork::match: Redirect decisions carry
    // either redirectUrl (data:/rewritten URL, ready to load) or a
    // canonical redirectResource name.
    AdBlockDecision check(const QUrl &url, const QUrl &firstPartyUrl,
                          int resourceType) const;

    // Cosmetic filtering payload for a document URL:
    // {"hide":[selectors...],"generichide":bool,"script":"..."}.
    // Empty object when no engine is loaded.
    QJsonObject cosmetic(const QUrl &documentUrl) const;

private:
    AdBlockRustEngine();
    Q_DISABLE_COPY(AdBlockRustEngine)

    AroraAdBlockEngine *m_engine;
    mutable QMutex m_mutex;
};

#endif // ADBLOCKRUSTENGINE_H
