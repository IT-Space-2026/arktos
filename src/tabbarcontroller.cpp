/*
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "tabbarcontroller.h"

using MenuEntry = TabBarController::MenuEntry;
using Id = MenuEntry::Id;

static MenuEntry entry(Id id)
{
    MenuEntry e;
    e.id = id;
    return e;
}

static MenuEntry tabEntry(Id id, int index)
{
    MenuEntry e = entry(id);
    e.index = index;
    return e;
}

QList<MenuEntry> TabBarController::contextMenuModel(
        const ContextMenuEnv &env)
{
    QList<MenuEntry> entries;

    // TABS05: undo-close — 'Reopen' pops the newest record, the
    // History-menu submenu lists the whole stack.  Reopen is enabled
    // only while the stack holds a record.
    entries << entry(Id::NewTab);
    MenuEntry reopen = entry(Id::ReopenClosedTab);
    reopen.enabled = env.hasRecentlyClosedTabs;
    entries << reopen
            << entry(Id::RecentlyClosedTabs)
            << entry(Id::Separator);

    if (env.onTab) {
        const int index = env.tabIndex;
        entries << tabEntry(Id::DuplicateTab, index);
        MenuEntry pin = tabEntry(Id::TogglePin, index);
        pin.flag = env.tabPinned;
        entries << pin
                << tabEntry(Id::RenameTab, index);

        // CONT02: "Reopen in Container" — off-the-record tabs never
        // move to persistent storage, and tor has no containers at
        // all, so the whole block hides under the private/tor gate.
        if (!env.privateMode && !env.torMode && !env.tabPrivate) {
            MenuEntry reopenIn = entry(Id::ReopenInContainerSubmenu);
            MenuEntry none = tabEntry(Id::ContainerItem, index);
            none.checkable = true;
            none.checked = env.currentContainerId.isEmpty();
            reopenIn.children << none;
            for (const ContextMenuEnv::Container &container
                 : env.containers) {
                MenuEntry item = tabEntry(Id::ContainerItem, index);
                item.checkable = true;
                item.checked = container.id == env.currentContainerId;
                item.key = container.id;
                item.text = container.name;
                item.color = container.color;
                reopenIn.children << item;
            }
            reopenIn.children << entry(Id::Separator)
                              << tabEntry(Id::NewContainer, index)
                              << tabEntry(Id::ManageContainers, index);
            entries << reopenIn;

            // CONT04: "Always open this site in <container>" — only
            // meaningful for host-bearing web pages.  A container tab
            // gets the flat checkbox on itself; a default tab picks
            // from every container in a submenu.
            if (!env.siteHost.isEmpty()) {
                if (!env.currentContainerId.isEmpty()) {
                    MenuEntry always =
                            tabEntry(Id::AlwaysInThisContainer, index);
                    always.checkable = true;
                    always.checked = env.siteRuleContainerId
                            == env.currentContainerId;
                    always.key = env.currentContainerId;
                    always.payload = env.siteHost;
                    entries << entry(Id::Separator) << always;
                } else if (!env.containers.isEmpty()) {
                    MenuEntry alwaysIn =
                            entry(Id::AlwaysInContainerSubmenu);
                    for (const ContextMenuEnv::Container &container
                         : env.containers) {
                        MenuEntry item =
                                tabEntry(Id::AlwaysInContainerItem,
                                         index);
                        item.checkable = true;
                        item.checked = env.siteRuleContainerId
                                == container.id;
                        item.key = container.id;
                        item.text = container.name;
                        item.payload = env.siteHost;
                        item.color = container.color;
                        alwaysIn.children << item;
                    }
                    entries << entry(Id::Separator) << alwaysIn;
                }
            }
        }

        // SLEEP01: sleeping the visible tab is impossible — the entry
        // disables on the current index; a suspended tab shows Wake.
        MenuEntry sleep = tabEntry(Id::SleepWakeTab, index);
        sleep.flag = env.tabSleeping;
        sleep.enabled = env.tabSleeping || !env.isCurrentTab;
        entries << sleep;

        // MEM08: the current tab's flush is a discard+reactivate — it
        // renders as the reload the user sees.  A slept tab's entry
        // stays live so the click can report the tab is already gone.
        MenuEntry free = tabEntry(Id::FreeTabMemory, index);
        free.flag = !env.tabSleeping && env.isCurrentTab;
        free.enabled = env.tabSleeping || env.canFreeMemory;
        entries << free << entry(Id::Separator);

        // TABGRP01: an ungrouped tab gets "Add Tab to Group"; a
        // grouped tab gets the group-level actions plus a way out.
        if (env.groupId.isEmpty()) {
            MenuEntry add = entry(Id::AddToGroupSubmenu);
            add.children << tabEntry(Id::NewGroupItem, index);
            if (!env.groups.isEmpty()) {
                add.children << entry(Id::Separator);
                for (const ContextMenuEnv::Group &group : env.groups) {
                    MenuEntry item = tabEntry(Id::GroupItem, index);
                    item.key = group.id;
                    item.text = group.name;
                    item.color = group.color;
                    add.children << item;
                }
            }
            entries << add;
        } else {
            MenuEntry collapsed = entry(Id::ToggleGroupCollapsed);
            collapsed.flag = env.groupCollapsed;
            collapsed.key = env.groupId;
            MenuEntry rename = entry(Id::RenameGroup);
            rename.key = env.groupId;
            rename.text = env.groupName;
            MenuEntry colors = entry(Id::GroupColorSubmenu);
            for (const QColor &swatch : env.groupPalette) {
                MenuEntry item = entry(Id::GroupColorItem);
                item.checkable = true;
                item.checked = swatch == env.groupColor;
                item.color = swatch;
                item.key = env.groupId;
                colors.children << item;
            }
            MenuEntry remove = tabEntry(Id::RemoveFromGroup, index);
            MenuEntry ungroup = entry(Id::Ungroup);
            ungroup.key = env.groupId;
            MenuEntry closeGroup = entry(Id::CloseGroup);
            closeGroup.key = env.groupId;
            entries << collapsed << rename << colors
                    << entry(Id::Separator)
                    << remove << ungroup << closeGroup;
        }

        entries << entry(Id::Separator)
                << tabEntry(Id::CloseTab, index)
                << tabEntry(Id::CloseOtherTabs, index)
                << entry(Id::Separator)
                << tabEntry(Id::ReloadTab, index);
    }

    MenuEntry freeAll = entry(Id::FreeBackgroundTabs);
    freeAll.enabled = env.tabCount > 1;
    entries << entry(Id::ReloadAllTabs)
            << freeAll
            << entry(Id::Separator)
            << entry(Id::BookmarkAllTabs)
            // TABS09: the icon-only view toggle is a strip-level entry
            // — offered whether or not the click landed on a tab.
            << entry(Id::Separator)
            << entry(Id::IconOnlyTabs);
    return entries;
}
