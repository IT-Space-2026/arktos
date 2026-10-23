/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef TABBARCONTROLLER_H
#define TABBARCONTROLLER_H

#include <qcolor.h>
#include <qlist.h>
#include <qstring.h>

// MS10: the decision side of TabBar's context menu — the sibling of
// MS04's WebViewController (which stays WebView-scoped by design).
// The widget keeps building/execing the QMenu and supplying the
// translated strings (paint); which entries exist, in which order,
// and their enabled/checked/label-variant state come from
// contextMenuModel().  The model is a pure function of the
// ContextMenuEnv the widget fills once per invocation — facade-bound
// reads where a surface exists today:
//
//   - container registry + site rules: ContainerManager — the store
//     of record, already riding the settings store rc_containers_*
//     operates on (its ProfileSettings group IS the facade surface;
//     no separate ac_containers handle exists Qt-side yet).
//   - reopen-enable (closed stack), pin/group reads: per-run widget
//     state that serializes through the AcSession manifest — MS08
//     verified no live facade surface applies; the Qt legs stay on
//     TabWidget's accessors (thin-blocked:MD08 for stack policy).
//   - private/tor mode + per-tab private gate: mode context legs —
//     the verdicts move to a profile-context facade surface when one
//     lands (same seam MS04 documents for its privateMode env leg).
//   - sleep/wake + free-memory enable: live engine-object state
//     (enginePage/url/lifecycle reads stay Qt-side by the thin-shell
//     rule); only the enable/label policy lives here.
class TabBarController
{
public:
    // Inputs the widget gathers once per menu invocation.  onTab is
    // false when the click landed past the last tab — the per-tab
    // block is skipped and only the strip-level rows render.
    struct ContextMenuEnv {
        // Strip-level
        bool hasRecentlyClosedTabs = false; // session closed-stack depth
        int tabCount = 0;                   // FreeBackgroundTabs enable

        // Per-tab context
        bool onTab = false;
        int tabIndex = -1;
        bool tabPinned = false;             // -> TogglePin label variant
        bool isCurrentTab = false;          // -> sleep/free gates
        bool privateMode = false;           // BrowserApplication::isPrivate()
        bool torMode = false;               // BrowserApplication::isTorMode()
        bool tabPrivate = false;            // PTAB01 per-tab kind

        // CONT02 containers — the registry rows and the tab's current
        // binding; CONT04's site rule inputs (host + ruled id).
        struct Container {
            QString id;
            QString name;
            QColor color;
        };
        QString currentContainerId;
        QList<Container> containers;
        QString siteHost;              // http/https host or empty
        QString siteRuleContainerId;   // containerIdForHost(siteHost)

        // SLEEP01 / MEM08 — engine lifecycle reads the widget took.
        bool tabSleeping = false;
        bool canFreeMemory = false;    // enginePage && !url.isEmpty()

        // TABGRP01 — the tab's group (empty = ungrouped) and every
        // existing group for the Add-submenu rows.
        struct Group {
            QString id;
            QString name;
            QColor color;
        };
        QString groupId;
        QString groupName;
        bool groupCollapsed = false;
        QColor groupColor;
        QList<Group> groups;
        QList<QColor> groupPalette;    // the 8 color-submenu swatches
    };

    struct MenuEntry {
        // One id per behavior the widget can wire — the model decides
        // the set/order/state, the widget maps ids to its slots and
        // produces the strings.
        enum class Id {
            Separator,
            // strip-level head — shared/shared-ish actions
            NewTab,                 // widget inserts newTabAction()
            ReopenClosedTab,        // enabled: closed stack non-empty
            RecentlyClosedTabs,     // widget inserts recentlyClosedTabsAction()
            // per-tab block
            DuplicateTab,
            TogglePin,              // flag: currently pinned -> "Unpin"
            RenameTab,
            ReopenInContainerSubmenu, // children: ContainerItem rows +
                                    // Separator/NewContainer/ManageContainers
            ContainerItem,          // key: container id ("" = default);
                                    // checkable+checked on the bound one
            NewContainer,
            ManageContainers,
            AlwaysInThisContainer,  // checkable; key: current id, payload: host
            AlwaysInContainerSubmenu, // children: AlwaysInContainerItem rows
            AlwaysInContainerItem,  // checkable; key: id, payload: host
            SleepWakeTab,           // flag: sleeping -> "Wake Tab" label
            FreeTabMemory,          // flag: reload variant (current+awake)
            AddToGroupSubmenu,      // children: NewGroupItem + GroupItem rows
            NewGroupItem,
            GroupItem,              // key: group id; text: name (widget
                                    // falls back to "Unnamed Group")
            ToggleGroupCollapsed,   // flag: collapsed -> "Expand Group"
            RenameGroup,            // key: group id; text: current name
            GroupColorSubmenu,      // children: GroupColorItem rows
            GroupColorItem,         // checkable; color: swatch; key: group id
            RemoveFromGroup,
            Ungroup,                // key: group id
            CloseGroup,             // key: group id
            CloseTab,
            CloseOtherTabs,
            ReloadTab,
            // strip-level tail
            ReloadAllTabs,
            FreeBackgroundTabs,     // enabled: tabCount > 1
            BookmarkAllTabs,        // widget inserts bookmarkTabsAction()
            IconOnlyTabs            // widget inserts iconOnlyTabsAction()
        };

        Id id;
        bool enabled = true;
        bool checkable = false;
        bool checked = false;
        // Semantic switch the widget maps to its label/behavior variant:
        // TogglePin -> pinned, SleepWakeTab -> sleeping, FreeTabMemory
        // -> reload variant, ToggleGroupCollapsed -> collapsed.
        bool flag = false;
        int index = -1;      // target tab index for per-tab entries
        QString key;         // container/group id
        QString text;        // display name / dialog seed
        QString payload;     // site-rule host
        QColor color;        // container/group swatch for the icon
        QList<MenuEntry> children;
    };

    // The full menu model for one context-menu request — entries in
    // display order, separators explicit.  The widget renders it
    // verbatim: shared QAction inserts where flagged, lambdas and
    // translated strings produced at render time.
    static QList<MenuEntry> contextMenuModel(const ContextMenuEnv &env);
};

#endif
