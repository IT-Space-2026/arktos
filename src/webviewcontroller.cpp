/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "webviewcontroller.h"

#include "engineinterface.h"

using MenuEntry = WebViewController::MenuEntry;
using Id = MenuEntry::Id;

static MenuEntry entry(Id id)
{
    MenuEntry e;
    e.id = id;
    return e;
}

static MenuEntry urlEntry(Id id, const QUrl &url, bool enabled = true)
{
    MenuEntry e = entry(id);
    e.url = url;
    e.enabled = enabled;
    return e;
}

// Cross-context open (private window/tab, tor hand-off): enabled only
// when the page-supplied url passed the CONT07 gate — entries that
// can never succeed are disabled at build time rather than refused on
// trigger.
static MenuEntry crossContextEntry(Id id, const QUrl &url, bool allowed)
{
    return urlEntry(id, url, allowed);
}

static MenuEntry torEntry(Id id, const QUrl &url, bool allowed,
                          bool torAvailable)
{
    MenuEntry e = urlEntry(id, url, allowed && torAvailable);
    if (!torAvailable)
        e.disabledReason = MenuEntry::DisabledReason::NoTorBinary;
    return e;
}

static MenuEntry pointEntry(Id id, const QPoint &position, bool canvas)
{
    MenuEntry e = entry(id);
    e.position = position;
    e.canvas = canvas;
    return e;
}

QList<MenuEntry> WebViewController::contextMenuModel(
        const Engine::ContextMenuInfo &info,
        const ContextMenuEnv &env)
{
    QList<MenuEntry> entries;
    const auto nonEmpty = [&entries]() { return !entries.isEmpty(); };

    if (!info.linkUrl.isEmpty()) {
        // CONT07: every entry hands an attacker-influenced page url to
        // a new browsing context — the slots re-gate the scheme
        // through isUrlAllowedFromPageLink, and entries that can never
        // succeed arrive disabled.
        const bool allowed = env.linkUrlAllowed;
        entries << urlEntry(Id::OpenLinkNewTab, info.linkUrl, allowed)
                << urlEntry(Id::OpenLinkNewWindow, info.linkUrl, allowed);
        // The private entries are hidden wherever they could not
        // honestly deliver a new isolated context: a fully private
        // window already routes every new tab/window through the OTR
        // profile, and inside a tor window they would have to land on
        // the CLEARNET OTR profile — a leak (privateMode covers tor
        // mode).  'Open in New Tor Window' always works — it spawns a
        // real --tor process.
        if (!env.privateMode) {
            entries << crossContextEntry(Id::OpenLinkNewPrivateTab,
                                         info.linkUrl, allowed)
                    << crossContextEntry(Id::OpenLinkNewPrivateWindow,
                                         info.linkUrl, allowed);
        }
        entries << torEntry(Id::OpenLinkNewTorWindow, info.linkUrl,
                            allowed, env.torAvailable);
        entries << entry(Id::Separator)
                << urlEntry(Id::SaveLink, info.linkUrl)
                << urlEntry(Id::BookmarkLink, info.linkUrl)
                << entry(Id::Separator);
        if (!info.selectedText.isEmpty())
            entries << entry(Id::InsertStandardCopy);
        entries << urlEntry(Id::CopyLinkLocation, info.linkUrl)
                // POL01: same copy but with tracking query parameters
                // removed.
                << urlEntry(Id::CopyCleanLink, info.linkUrl);
    }

    if (info.hasImage && !info.mediaUrl.isEmpty()) {
        if (nonEmpty())
            entries << entry(Id::Separator);
        // The same-profile opens keep the permissive base gate — a
        // blob: media url is only resolvable inside this profile.  The
        // private/tor entries hand the page-supplied url to another
        // context and take the strict page-link gate (CONT07).
        const bool allowed = env.mediaUrlAllowed;
        entries << urlEntry(Id::OpenImageNewTab, info.mediaUrl)
                << urlEntry(Id::OpenImageNewWindow, info.mediaUrl);
        if (!env.privateMode) {
            entries << crossContextEntry(Id::OpenImageNewPrivateTab,
                                         info.mediaUrl, allowed)
                    << crossContextEntry(Id::OpenImageNewPrivateWindow,
                                         info.mediaUrl, allowed);
        }
        entries << torEntry(Id::OpenImageNewTorWindow, info.mediaUrl,
                            allowed, env.torAvailable);
        MenuEntry copyLocation = urlEntry(Id::CopyImageLocation,
                                          info.mediaUrl);
        copyLocation.text = info.mediaUrl.toString();
        MenuEntry block = urlEntry(Id::BlockImage, info.mediaUrl);
        block.text = info.mediaUrl.toString();
        entries << entry(Id::Separator)
                << urlEntry(Id::SaveImage, info.mediaUrl)
                << entry(Id::CopyImage)
                << copyLocation
                << entry(Id::Separator)
                << block;
        // SRCH04: reverse-image search through the configured image
        // engine — only offered when an engine actually advertises an
        // image-search endpoint.
        if (!env.imageSearchEngineName.isEmpty()) {
            MenuEntry search = urlEntry(Id::ImageSearch, info.mediaUrl);
            search.text = env.imageSearchEngineName;
            entries << search;
        }
    } else if (info.hasImage || info.isCanvas) {
        // CTX01: a <canvas> reports no mediaUrl (its pixels only exist
        // in page memory) and an <img> occasionally arrives with an
        // empty one — the content under the click point is resolved
        // in-page via contextimage.js.  Canvas grabs hand back a png
        // data: url, so open/save share the url-based paths; a canvas
        // has no location to copy or block rule to write.
        if (nonEmpty())
            entries << entry(Id::Separator);
        const QPoint position = info.position;
        const bool canvas = info.isCanvas;
        entries << pointEntry(Id::GrabOpenImageWindow, position, canvas)
                << pointEntry(Id::GrabOpenImageTab, position, canvas)
                << entry(Id::Separator)
                << pointEntry(Id::GrabSaveImage, position, canvas);
        if (canvas) {
            entries << pointEntry(Id::GrabCopyCanvasImage, position,
                                  true);
        } else {
            entries << entry(Id::CopyImage)
                    << pointEntry(Id::GrabCopyImageLocation, position,
                                  false)
                    << entry(Id::Separator)
                    << pointEntry(Id::GrabBlockImage, position, false);
        }
    }

    // PIP01: right-click on a <video> offers the pop-out.  The element
    // itself is resolved in-page (click point + media url); the poster
    // frame is the video's image — the request carries only the stream
    // url, so the poster attribute is resolved in-page too.
    if (info.hasVideo) {
        if (nonEmpty())
            entries << entry(Id::Separator);
        MenuEntry pip = urlEntry(Id::PictureInPicture, info.mediaUrl);
        pip.position = info.position;
        entries << pip
                << pointEntry(Id::OpenPosterNewWindow, info.position,
                              false)
                << pointEntry(Id::OpenPosterNewTab, info.position,
                              false);
    }

    if (!info.selectedText.isEmpty()) {
        if (entries.isEmpty()) {
            entries << entry(Id::InsertStandardCopy);
        } else {
            entries << entry(Id::Separator);
        }
        MenuEntry searchWith = entry(Id::SearchWithSubmenu);
        for (const QString &name : env.searchEngineNames) {
            MenuEntry engine = entry(Id::SearchEngineItem);
            engine.text = name;
            searchWith.children << engine;
        }
        entries << searchWith
                << entry(Id::Separator)
                // NOTE03: the selection lands as a new note in this
                // window's Notes panel.
                << entry(Id::AddSelectionToNotes);
    }

    // SRCH01: 'Add to the toolbar search' — under WebEngine the
    // editable flag is all the request carries, so the entry shows for
    // any editable spot and the contextsearch.js probe decides on
    // trigger whether a GET form actually sits there.
    if (info.isContentEditable) {
        if (entries.isEmpty()) {
            // A bare editable click keeps Chromium's editing commands
            // (Cut/Copy/Paste/Select All...) so the provider entry
            // doesn't cost the user those.
            entries << entry(Id::InsertStandardActions);
        }
        if (nonEmpty())
            entries << entry(Id::Separator);
        entries << pointEntry(Id::AddToolbarSearch, info.position,
                              false);
    }

    if (entries.isEmpty())
        entries << entry(Id::InsertStandardActions);

    if (nonEmpty())
        entries << entry(Id::Separator);
    // POL01: on a link menu 'Copy Clean Link' was already added
    // against the link url above; here it cleans the page address.
    if (info.linkUrl.isEmpty())
        entries << entry(Id::CopyCleanPageLink);
    entries << entry(Id::ShowQrCode);

    return entries;
}

QString WebViewController::hoverStatusText(const QString &hoveredLink)
{
    return hoveredLink;
}
