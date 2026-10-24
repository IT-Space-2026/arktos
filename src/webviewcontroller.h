/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef WEBVIEWCONTROLLER_H
#define WEBVIEWCONTROLLER_H

#include <qlist.h>
#include <qpoint.h>
#include <qstring.h>
#include <qstringlist.h>
#include <qurl.h>

namespace Engine { struct ContextMenuInfo; }

// MS04: the decision side of WebView's context menu and link-hover
// status text.  The widget keeps building/execing the QMenu and
// supplying the translated strings (paint); which entries exist, in
// which order, and their enabled/tooltip state come from
// contextMenuModel().  All methods are pure functions of the
// ContextMenuInfo payload plus an Environment the widget fills from
// its stores — facade-bound reads where a surface exists today:
//
//   - opensearch engine list + image-engine presence: rc_ose-bound
//     through OpenSearchManager (already the store of record).
//   - private-mode and the page-link scheme gate: Qt legs — the
//     verdicts move to MD02 navpolicy / profile context facade when
//     those land (thin-blocked, see .devin/MW-SHELL.md).
//   - tor-binary availability: TorManager::resolveBinary() probe.
//     ac_engine_caps (MS05 port of MB04) does NOT cover it — the
//     `tor` row grades per-engine SOCKS5 support (always
//     qtwebengine here), never whether a daemon/binary resolves.
class WebViewController
{
public:
    // Inputs the widget gathers once per menu invocation.
    struct ContextMenuEnv {
        bool privateMode = false;       // BrowserApplication::isPrivate()
        bool torAvailable = false;      // resolveBinary() probe — not caps
        // isUrlAllowedFromPageLink() on info.linkUrl / info.mediaUrl —
        // the CONT07 cross-context gate (Qt leg until MD02).
        bool linkUrlAllowed = false;
        bool mediaUrlAllowed = false;
        // Empty when no engine advertises an image-search endpoint.
        QString imageSearchEngineName;
        QStringList searchEngineNames;  // 'Search with' submenu rows
    };

    struct MenuEntry {
        // One id per behavior the widget can wire — the model decides
        // the set/order/state, the widget maps ids to its slots.
        enum class Id {
            Separator,
            // link-target block (data: link url)
            OpenLinkNewTab,
            OpenLinkNewWindow,
            OpenLinkNewPrivateTab,
            OpenLinkNewPrivateWindow,
            OpenLinkNewTorWindow,
            SaveLink,
            BookmarkLink,
            InsertStandardCopy,   // enginePage()->action(Copy)
            CopyLinkLocation,
            CopyCleanLink,
            // image block when mediaUrl resolves (data: media url)
            OpenImageNewTab,
            OpenImageNewWindow,
            OpenImageNewPrivateTab,
            OpenImageNewPrivateWindow,
            OpenImageNewTorWindow,
            SaveImage,
            CopyImage,
            CopyImageLocation,    // data: media url STRING (text field)
            BlockImage,           // data: media url string (text field)
            ImageSearch,          // text: engine name, data: media url
            // canvas / image-without-mediaUrl block — the content is
            // resolved in-page at the stored click point (CTX01)
            GrabOpenImageWindow,
            GrabOpenImageTab,
            GrabSaveImage,
            GrabCopyCanvasImage,
            GrabCopyImageLocation,
            GrabBlockImage,
            // video block
            PictureInPicture,     // data: media url + position
            OpenPosterNewWindow,
            OpenPosterNewTab,
            // selection block
            SearchWithSubmenu,    // children: SearchEngineItem rows
            SearchEngineItem,     // text: engine name (only in children)
            AddSelectionToNotes,
            // editable / empty-menu markers — the widget expands them
            // to Chromium's standard actions minus InspectElement
            InsertStandardActions,
            AddToolbarSearch,
            // page tail
            CopyCleanPageLink,
            ShowQrCode
        };
        // Why a disabled entry is disabled — the widget maps the
        // reason to its translated tooltip so the text keeps its
        // WebView lupdate context.
        enum class DisabledReason {
            None,
            NoTorBinary
        };

        Id id;
        bool enabled = true;
        DisabledReason disabledReason = DisabledReason::None;
        QUrl url;               // action data for url-based entries
        QString text;           // url-string data / engine-name label
        QPoint position;        // click point for in-page resolvers
        bool canvas = false;    // grab entries: canvas vs <img>
        QList<MenuEntry> children;
    };

    // The full menu model for one context-menu request — entries in
    // display order, separators explicit.  The widget renders it
    // verbatim, then appends chrome-side rows (extension
    // contributions, the show-menubar action) that are widget state,
    // not page-content decisions.
    static QList<MenuEntry> contextMenuModel(
            const Engine::ContextMenuInfo &info,
            const ContextMenuEnv &env);

    // Hover-status policy: what the status bar shows for a hovered
    // link.  Today: the link verbatim (parity with the pass-through
    // this replaces) — this is the seam where display rules (decoded
    // vs raw form, scheme suppression) would consult the facade.
    static QString hoverStatusText(const QString &hoveredLink);
};

#endif
