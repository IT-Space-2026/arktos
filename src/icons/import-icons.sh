#!/bin/bash
# Regenerates the bundled icon themes under this directory.
#
# Sources (see THIRD-PARTY-NOTICES for licensing):
#   Adwaita   — /usr/share/icons/Adwaita (or gitlab.gnome.org/GNOME/
#               adwaita-icon-theme); symbolic, monochrome — a -dark
#               recolor is generated next to it.
#   Breeze    — github.com/KDE/breeze-icons (icons/actions|places/16);
#               monochrome ColorScheme-Text — a -dark recolor is
#               generated next to it.
#   Tabler    — github.com/tabler/tabler-icons (icons/outline);
#               stroke=currentColor is bound to a concrete gray for
#               light and #e8e8e8 for -dark.
#   Lucide    — github.com/lucide-icons/lucide (icons/, or the
#               lucide-static npm package); ISC, Feather-descended.
#               Same 24px/2px currentColor conventions as Tabler, so
#               the -dark recolor is generated identically.
#
# Qt renders currentColor as black (it does not recolor against the
# widget palette), so every bundled monochrome glyph is pinned to a
# concrete color per variant.
#
# Usage: ./import-icons.sh <adwaita-dir> <breeze-icons-dir> <tabler-dir> <lucide-dir>
set -euo pipefail

ADWAITA=${1:-/usr/share/icons/Adwaita}
BREEZE=${2:?usage: import-icons.sh <adwaita> <breeze-icons> <tabler> <lucide>}
TABLER=${3:?usage: import-icons.sh <adwaita> <breeze-icons> <tabler> <lucide>}
LUCIDE=${4:?usage: import-icons.sh <adwaita> <breeze-icons> <tabler> <lucide>}
OUT=$(dirname "$0")

# freedesktop name -> file per set
declare -A ADWAITA_MAP=(
    [window-new]=symbolic/ui/window-new-symbolic.svg
    [window-close]=symbolic/ui/window-close-symbolic.svg
    [document-open]=symbolic/actions/document-open-symbolic.svg
    [document-print]=symbolic/actions/document-print-symbolic.svg
    [document-print-preview]=symbolic/actions/document-print-preview-symbolic.svg
    [document-save-as]=symbolic/actions/document-save-as-symbolic.svg
    [document-revert]=symbolic/actions/document-revert-symbolic.svg
    [application-exit]=symbolic/actions/application-exit-symbolic.svg
    [edit-undo]=symbolic/actions/edit-undo-symbolic.svg
    [edit-redo]=symbolic/actions/edit-redo-symbolic.svg
    [edit-cut]=symbolic/actions/edit-cut-symbolic.svg
    [edit-copy]=symbolic/actions/edit-copy-symbolic.svg
    [edit-paste]=symbolic/actions/edit-paste-symbolic.svg
    [edit-select-all]=symbolic/actions/edit-select-all-symbolic.svg
    [edit-find]=symbolic/actions/edit-find-symbolic.svg
    [edit-clear]=symbolic/actions/edit-clear-symbolic.svg
    [edit-clear-locationbar-ltr]=symbolic/actions/edit-clear-symbolic.svg
    [edit-clear-locationbar-rtl]=symbolic/actions/edit-clear-symbolic.svg
    [go-previous]=symbolic/actions/go-previous-symbolic.svg
    [go-next]=symbolic/actions/go-next-symbolic.svg
    [go-home]=symbolic/actions/go-home-symbolic.svg
    [process-stop]=symbolic/actions/process-stop-symbolic.svg
    # ICONS04: view-refresh is NOT imported — the stock Adwaita glyph is
    # a filled arrow ~2x heavier than the chevrons; a maintained
    # weight-matched glyph is written by write_reload() below.
    [view-fullscreen]=symbolic/actions/view-fullscreen-symbolic.svg
    [zoom-in]=symbolic/actions/zoom-in-symbolic.svg
    [zoom-out]=symbolic/actions/zoom-out-symbolic.svg
    [zoom-original]=symbolic/actions/zoom-original-symbolic.svg
    [bookmark-new]=symbolic/actions/bookmark-new-symbolic.svg
    [user-bookmarks]=symbolic/places/user-bookmarks-symbolic.svg
    [folder-new]=symbolic/actions/folder-new-symbolic.svg
    [emblem-downloads]=symbolic/places/folder-download-symbolic.svg
    [preferences-desktop-locale]=symbolic/legacy/preferences-desktop-locale-symbolic.svg
    [tab-new]=symbolic/actions/tab-new-symbolic.svg
    [list-add]=symbolic/actions/list-add-symbolic.svg
    [system-run]=symbolic/actions/system-run-symbolic.svg
)
declare -A BREEZE_MAP=(
    [window-new]=icons/actions/16/window-new.svg
    [window-close]=icons/actions/16/window-close.svg
    [document-open]=icons/actions/16/document-open.svg
    [document-print]=icons/actions/16/document-print.svg
    [document-print-preview]=icons/actions/16/document-print-preview.svg
    [document-save-as]=icons/actions/16/document-save-as.svg
    [document-revert]=icons/actions/16/document-revert.svg
    [application-exit]=icons/actions/16/application-exit.svg
    [edit-undo]=icons/actions/16/edit-undo.svg
    [edit-redo]=icons/actions/16/edit-redo.svg
    [edit-cut]=icons/actions/16/edit-cut.svg
    [edit-copy]=icons/actions/16/edit-copy.svg
    [edit-paste]=icons/actions/16/edit-paste.svg
    [edit-select-all]=icons/actions/16/edit-select-all.svg
    [edit-find]=icons/actions/16/edit-find.svg
    [edit-clear]=icons/actions/16/edit-clear.svg
    [edit-clear-locationbar-ltr]=icons/actions/16/edit-clear-locationbar-ltr.svg
    [edit-clear-locationbar-rtl]=icons/actions/16/edit-clear-locationbar-rtl.svg
    [go-previous]=icons/actions/16/go-previous.svg
    [go-next]=icons/actions/16/go-next.svg
    [go-home]=icons/actions/16/go-home.svg
    # ICONS04: upstream process-stop is a NegativeText (red) glyph —
    # a maintained weight-matched X is written by write_stop() below.
    # view-refresh is a maintained glyph — see write_reload() below.
    [view-fullscreen]=icons/actions/16/view-fullscreen.svg
    [zoom-in]=icons/actions/16/zoom-in.svg
    [zoom-out]=icons/actions/16/zoom-out.svg
    [zoom-original]=icons/actions/16/zoom-original.svg
    [bookmark-new]=icons/actions/16/bookmark-new.svg
    [user-bookmarks]=icons/places/16/user-bookmarks-symbolic.svg
    [folder-new]=icons/actions/16/folder-new.svg
    [emblem-downloads]=icons/actions/16/download.svg
    [preferences-desktop-locale]=icons/actions/16/set-language.svg
    [tab-new]=icons/actions/16/tab-new.svg
    # ICONS10: tab-close is real upstream art in breeze — File >
    # Close Tab and Close Window used to share window-close's glyph.
    [tab-close]=icons/actions/16/tab-close.svg
    [list-add]=icons/actions/16/list-add.svg
    [system-run]=icons/actions/16/system-run.svg
)
declare -A TABLER_MAP=(
    [window-new]=browser-plus
    [window-close]=x
    [document-open]=folder-open
    [document-print]=printer
    [document-print-preview]=file-search
    [document-save-as]=device-floppy
    [document-revert]=restore
    [application-exit]=logout
    [edit-undo]=arrow-back-up
    [edit-redo]=arrow-forward-up
    [edit-cut]=scissors
    [edit-copy]=copy
    [edit-paste]=clipboard
    [edit-select-all]=select-all
    [edit-find]=search
    [edit-clear]=circle-x
    [edit-clear-locationbar-ltr]=circle-x
    [edit-clear-locationbar-rtl]=circle-x
    [go-previous]=arrow-left
    [go-next]=arrow-right
    [go-home]=home
    # ICONS04: plain x, not square-x — the box tripled the ink next to
    # the thin arrows.  view-refresh is a maintained glyph — see
    # write_reload() below.
    # ICONS10: player-stop, not x — window-close already took x and
    # identical glyphs were the "icons look the same" report.
    [process-stop]=player-stop
    [view-fullscreen]=arrows-maximize
    [zoom-in]=zoom-in
    [zoom-out]=zoom-out
    [zoom-original]=zoom-reset
    [bookmark-new]=bookmark-plus
    [user-bookmarks]=bookmarks
    [folder-new]=folder-plus
    [emblem-downloads]=download
    [preferences-desktop-locale]=language
    [tab-new]=plus
    # ICONS10: browser-x so File > Close Tab does not share
    # window-close's bare x.
    [tab-close]=browser-x
    # ICONS10: playlist-add, not plus — tab-new already took the bare
    # plus and identical glyphs were the "icons look the same" report.
    # Same for player-stop: a stop square distinct from window-close's x.
    [list-add]=playlist-add
    [system-run]=player-play
    # ICONS10 coverage audit — every chrome name the code requests
    # resolves inside this set; tabler is the fallback root every
    # other set inherits.
    [accessories-character-map]=omega
    [applications-development]=device-desktop-code
    [application-x-addon]=puzzle
    [avatar-default]=user-circle
    [container-tab]=container
    [document-export]=file-export
    [document-import]=file-import
    [go-down]=arrow-down
    [go-jump]=arrow-bar-to-right
    [go-up]=arrow-up
    [preferences-system]=settings
    [security-high]=shield-check
    [text-html]=code
    [text-x-generic]=article
    [utilities-terminal]=terminal-2
    [view-history]=history
    [view-icon]=icons
    [view-menubar]=layout-navbar
    [view-pip]=picture-in-picture
    [view-private]=spy
    [view-restore]=stack-pop
    [view-statusbar]=layout-bottombar
    [view-tabbar]=browser
    [view-toolbar]=tools
    [zoom-text]=text-size
)
# ICONS11: Lucide covers the same 62-name chrome surface.  Lucide is a
# Feather descendant at the same conventions as Tabler (24px box, 2px
# round-cap currentColor strokes), so it is the closest open-source
# match to Firefox's Proton icon language.  Where Lucide ships no
# direct equivalent the map picks the documented closest glyph —
# substitutions are recorded below; the rest are one-to-one.
declare -A LUCIDE_MAP=(
    # window-new: lucide has no window-plus — app-window is the bare
    # window glyph (the "new" is carried by the menu text).
    [window-new]=app-window
    [window-close]=x
    [document-open]=folder-open
    [document-print]=printer
    # document-print-preview: file-search = magnifier over a document.
    [document-print-preview]=file-search
    # document-save-as: save is the classic floppy.
    [document-save-as]=save
    # document-revert: rotate-ccw is the counter-clockwise "revert".
    [document-revert]=rotate-ccw
    [application-exit]=log-out
    [edit-undo]=undo-2
    [edit-redo]=redo-2
    [edit-cut]=scissors
    [edit-copy]=copy
    # edit-paste: clipboard-paste is a clipboard with an incoming
    # arrow — the paste action, not the bare clipboard.
    [edit-paste]=clipboard-paste
    # edit-select-all: square-dashed is the dashed selection marquee.
    [edit-select-all]=square-dashed
    [edit-find]=search
    [edit-clear]=circle-x
    [edit-clear-locationbar-ltr]=circle-x
    [edit-clear-locationbar-rtl]=circle-x
    [go-previous]=arrow-left
    [go-next]=arrow-right
    # go-home: lucide renamed home -> house; house.svg is canonical.
    [go-home]=house
    # process-stop: square is lucide's media-stop glyph — kept, not
    # x, so it stays distinct from window-close (the ICONS10 lesson).
    [process-stop]=square
    # view-fullscreen: maximize is the four outward corner brackets.
    [view-fullscreen]=maximize
    [zoom-in]=zoom-in
    [zoom-out]=zoom-out
    # zoom-original: lucide ships no 1:1 glyph — scan-search is the
    # magnifier inside the scan frame ("inspect at real size").
    [zoom-original]=scan-search
    [bookmark-new]=bookmark-plus
    # user-bookmarks: lucide has no bookmarks plural — book-marked is
    # a book carrying a bookmark ribbon, distinct from bookmark-plus.
    [user-bookmarks]=book-marked
    [folder-new]=folder-plus
    [emblem-downloads]=download
    [preferences-desktop-locale]=languages
    [tab-new]=plus
    # tab-close: panel-top-close is a top strip with an x — a closing
    # tab, distinct from window-close's bare x.
    [tab-close]=panel-top-close
    [list-add]=list-plus
    [system-run]=play
    [accessories-character-map]=omega
    # applications-development: braces ({ }) reads as development and
    # stays distinct from text-html's code (</>) below.
    [applications-development]=braces
    [application-x-addon]=puzzle
    [avatar-default]=circle-user-round
    [container-tab]=container
    [document-export]=file-output
    [document-import]=file-input
    [go-down]=arrow-down
    [go-jump]=arrow-right-to-line
    [go-up]=arrow-up
    [preferences-system]=settings
    [security-high]=shield-check
    [text-html]=code
    [text-x-generic]=file-text
    [utilities-terminal]=square-terminal
    [view-history]=history
    [view-icon]=layout-grid
    # The View-menu bar toggles form one positional family:
    # menubar=layout-panel-top, tabbar=panel-top, statusbar=
    # panel-bottom — same metaphor, three distinct shapes.
    [view-menubar]=layout-panel-top
    [view-pip]=picture-in-picture-2
    # view-private: hat-glasses is lucide's incognito glyph.
    [view-private]=hat-glasses
    # view-restore: archive-restore (tray + upward arrow) is the
    # bring-back glyph — recently-closed tabs and the PiP
    # return-to-tab button.
    [view-restore]=archive-restore
    [view-statusbar]=panel-bottom
    [view-tabbar]=panel-top
    [view-toolbar]=wrench
    [zoom-text]=a-large-small
    # view-refresh and tor-onion are maintained glyphs — see
    # write_reload()/write_onion() below; lucide ships no onion and
    # its refresh-cw is heavier than the nav arrows.
)

copy_map() { # assoc-name srcroot outdir [sed-expr...]
    local -n map=$1; local src=$2; local dst=$3; shift 3
    mkdir -p "$dst/icons"
    for name in "${!map[@]}"; do
        local f="$src/${map[$name]}"
        [ -f "$f" ] || f="$f.svg"
        [ -f "$f" ] || { echo "MISSING $src/${map[$name]}" >&2; exit 1; }
        if [ $# -gt 0 ]; then
            sed "$@" "$f" > "$dst/icons/$name.svg"
        else
            cp "$f" "$dst/icons/$name.svg"
        fi
    done
}

index_theme() { # dir name comment [inherits]
    # ICONS13: sparse sets inherit arora-tabler (the full-coverage
    # set) — dark variants inherit arora-tabler-dark so a missed name
    # resolves to light-stroke art on dark chrome.
    {
        cat <<EOF
[Icon Theme]
Name=$2
Comment=$3
Directories=icons
EOF
        [ -n "${4:-}" ] && echo "Inherits=$4"
        cat <<'EOF'

[icons]
Size=16
Type=Scalable
MinSize=8
MaxSize=256
EOF
    } > "$1/index.theme"
}

# ICONS04: view-refresh is a maintained glyph, not an upstream copy —
# Adwaita ships a filled arrow, Breeze a two-arrow composite and Tabler
# a double-arrowhead glyph, all ~2-3x heavier than the sets' nav arrows.
# One circular-arrow geometry per viewBox, stroked at each set's weight.
write_reload() { # outdir color width extent arc head
    cat > "$1/icons/view-refresh.svg" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!-- ICONS04: hand-drawn circular arrow matching the nav arrows' weight
     (the stock glyphs carried 2-3x the ink of the arrows). -->
<svg height="$4" viewBox="0 0 $4 $4" width="$4" xmlns="http://www.w3.org/2000/svg">
    <g fill="none" stroke="$2" stroke-width="$3" stroke-linecap="round" stroke-linejoin="round">
        <path d="$5"/>
        <path d="$6"/>
    </g>
</svg>
EOF
}
RELOAD_ARC16="m 11.25 5.05 a 4.6 4.6 0 1 0 -0.95 7.23"
RELOAD_HEAD16="m 10.72 2.72 l 0.53 2.33 l -2.38 -0.13"
RELOAD_ARC24="m 16.88 7.57 a 6.9 6.9 0 1 0 -1.43 10.85"
RELOAD_HEAD24="m 16.08 4.08 l 0.8 3.49 l -3.57 -0.2"

# ICONS10: tor-onion has no upstream tabler glyph — a maintained
# onion outline at tabler conventions (24 box, 2px round-cap stroke):
# concentric bulbs plus the sprout.
write_onion() { # outdir color
    cat > "$1/icons/tor-onion.svg" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!-- ICONS10: hand-drawn onion matching tabler conventions — upstream
     ships no onion glyph. -->
<svg height="24" viewBox="0 0 24 24" width="24" xmlns="http://www.w3.org/2000/svg">
    <g fill="none" stroke="$2" stroke-width="2" stroke-linecap="round" stroke-linejoin="round">
        <path d="M12 6c-4.2 1.6-7 5.6-7 9.5a7 7 0 0 0 14 0c0-3.9-2.8-7.9-7-9.5z"/>
        <path d="M12 6c-2.4 1.4-4 4.9-4 8.4a4 4 0 0 0 8 0c0-3.5-1.6-7-4-8.4z"/>
        <path d="M12 6v-1.8c0-1 .8-1.7 1.8-1.7"/>
        <path d="M12 10.4c-1 1-1.6 2.4-1.6 4a1.6 1.6 0 0 0 3.2 0c0-1.6-.6-3-1.6-4z"/>
    </g>
</svg>
EOF
}

# ICONS04: breeze's process-stop gets the same treatment — upstream is
# a colored NegativeText glyph; a plain X at set weight keeps the nav
# cluster monochrome.
write_stop() { # outdir color width
    cat > "$1/icons/process-stop.svg" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!-- ICONS04: plain X matching the nav arrows' weight — upstream
     process-stop is a colored (NegativeText) glyph. -->
<svg height="16" viewBox="0 0 16 16" width="16" xmlns="http://www.w3.org/2000/svg">
    <g fill="none" stroke="$2" stroke-width="$3" stroke-linecap="round">
        <path d="m 4.2 4.2 l 7.6 7.6 m -7.6 0 l 7.6 -7.6"/>
    </g>
</svg>
EOF
}

# --- light variants ---------------------------------------------------
copy_map ADWAITA_MAP "$ADWAITA" "$OUT/arora-adwaita"
copy_map BREEZE_MAP  "$BREEZE"  "$OUT/arora-breeze"
copy_map TABLER_MAP  "$TABLER"  "$OUT/arora-tabler" \
    -e 's/stroke="currentColor"/stroke="#3d3d3d"/g'
copy_map LUCIDE_MAP  "$LUCIDE"  "$OUT/arora-lucide" \
    -e 's/stroke="currentColor"/stroke="#3d3d3d"/g'
write_reload "$OUT/arora-adwaita" "#2e3436" 1.7 16 "$RELOAD_ARC16" "$RELOAD_HEAD16"
write_reload "$OUT/arora-breeze"  "#232629" 1   16 "$RELOAD_ARC16" "$RELOAD_HEAD16"
write_reload "$OUT/arora-tabler"  "#3d3d3d" 2   24 "$RELOAD_ARC24" "$RELOAD_HEAD24"
write_reload "$OUT/arora-lucide"  "#3d3d3d" 2   24 "$RELOAD_ARC24" "$RELOAD_HEAD24"
write_onion  "$OUT/arora-tabler"  "#3d3d3d"
write_onion  "$OUT/arora-lucide"  "#3d3d3d"
write_stop   "$OUT/arora-breeze"  "#232629" 1

index_theme "$OUT/arora-adwaita" "Arora Adwaita" \
    "Bundled Adwaita symbolic icons (CC-BY-SA-3.0/LGPL-3+ or CC-BY-SA-4.0)" \
    arora-tabler
index_theme "$OUT/arora-breeze" "Arora Breeze" \
    "Bundled KDE Breeze icons (LGPL-3+)" \
    arora-tabler
index_theme "$OUT/arora-tabler" "Arora Tabler" \
    "Bundled Tabler outline icons (MIT)"
index_theme "$OUT/arora-lucide" "Arora Lucide" \
    "Bundled Lucide icons (ISC; partly derived from Feather, MIT)" \
    arora-tabler

# --- dark variants ----------------------------------------------------
copy_map ADWAITA_MAP "$ADWAITA" "$OUT/arora-adwaita-dark" \
    -e 's/#2e3436/#e8e8e8/g' -e 's/#2e3434/#e8e8e8/g' -e 's/#505050/#b0b0b0/g'
copy_map BREEZE_MAP  "$BREEZE"  "$OUT/arora-breeze-dark" \
    -e 's/#232629/#eff0f1/g' -e 's/#666666/#a0a0a0/g'
copy_map TABLER_MAP  "$TABLER"  "$OUT/arora-tabler-dark" \
    -e 's/stroke="currentColor"/stroke="#e8e8e8"/g'
copy_map LUCIDE_MAP  "$LUCIDE"  "$OUT/arora-lucide-dark" \
    -e 's/stroke="currentColor"/stroke="#e8e8e8"/g'
write_reload "$OUT/arora-adwaita-dark" "#e8e8e8" 1.7 16 "$RELOAD_ARC16" "$RELOAD_HEAD16"
write_reload "$OUT/arora-breeze-dark"  "#eff0f1" 1   16 "$RELOAD_ARC16" "$RELOAD_HEAD16"
write_reload "$OUT/arora-tabler-dark"  "#e8e8e8" 2   24 "$RELOAD_ARC24" "$RELOAD_HEAD24"
write_reload "$OUT/arora-lucide-dark"  "#e8e8e8" 2   24 "$RELOAD_ARC24" "$RELOAD_HEAD24"
write_onion  "$OUT/arora-tabler-dark"  "#e8e8e8"
write_onion  "$OUT/arora-lucide-dark"  "#e8e8e8"
write_stop   "$OUT/arora-breeze-dark"  "#eff0f1" 1

index_theme "$OUT/arora-adwaita-dark" "Arora Adwaita Dark" \
    "Dark-scheme recolor of the bundled Adwaita symbolic icons" \
    arora-tabler-dark
index_theme "$OUT/arora-breeze-dark" "Arora Breeze Dark" \
    "Dark-scheme recolor of the bundled KDE Breeze icons" \
    arora-tabler-dark
index_theme "$OUT/arora-tabler-dark" "Arora Tabler Dark" \
    "Dark-scheme recolor of the bundled Tabler outline icons" \
    arora-tabler
index_theme "$OUT/arora-lucide-dark" "Arora Lucide Dark" \
    "Dark-scheme recolor of the bundled Lucide icons" \
    arora-tabler-dark

echo "bundled icon themes regenerated in $OUT"
