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
#
# Qt renders currentColor as black (it does not recolor against the
# widget palette), so every bundled monochrome glyph is pinned to a
# concrete color per variant.
#
# Usage: ./import-icons.sh <adwaita-dir> <breeze-icons-dir> <tabler-dir>
set -euo pipefail

ADWAITA=${1:-/usr/share/icons/Adwaita}
BREEZE=${2:?usage: import-icons.sh <adwaita> <breeze-icons> <tabler>}
TABLER=${3:?usage: import-icons.sh <adwaita> <breeze-icons> <tabler>}
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
    [view-refresh]=symbolic/actions/view-refresh-symbolic.svg
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
    [process-stop]=icons/actions/16/process-stop.svg
    [view-refresh]=icons/actions/16/view-refresh.svg
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
    [process-stop]=square-x
    [view-refresh]=refresh
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
    [list-add]=plus
    [system-run]=player-play
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

index_theme() { # dir name comment
    cat > "$1/index.theme" <<EOF
[Icon Theme]
Name=$2
Comment=$3
Directories=icons
Inherits=arora-legacy

[icons]
Size=16
Type=Scalable
MinSize=8
MaxSize=256
EOF
}

# --- light variants ---------------------------------------------------
copy_map ADWAITA_MAP "$ADWAITA" "$OUT/arora-adwaita"
copy_map BREEZE_MAP  "$BREEZE"  "$OUT/arora-breeze"
copy_map TABLER_MAP  "$TABLER"  "$OUT/arora-tabler" \
    -e 's/stroke="currentColor"/stroke="#3d3d3d"/g'

index_theme "$OUT/arora-adwaita" "Arora Adwaita" \
    "Bundled Adwaita symbolic icons (CC-BY-SA-3.0/LGPL-3+ or CC-BY-SA-4.0)"
index_theme "$OUT/arora-breeze" "Arora Breeze" \
    "Bundled KDE Breeze icons (LGPL-3+)"
index_theme "$OUT/arora-tabler" "Arora Tabler" \
    "Bundled Tabler outline icons (MIT)"

# --- dark variants ----------------------------------------------------
copy_map ADWAITA_MAP "$ADWAITA" "$OUT/arora-adwaita-dark" \
    -e 's/#2e3436/#e8e8e8/g' -e 's/#2e3434/#e8e8e8/g' -e 's/#505050/#b0b0b0/g'
copy_map BREEZE_MAP  "$BREEZE"  "$OUT/arora-breeze-dark" \
    -e 's/#232629/#eff0f1/g' -e 's/#666666/#a0a0a0/g'
copy_map TABLER_MAP  "$TABLER"  "$OUT/arora-tabler-dark" \
    -e 's/stroke="currentColor"/stroke="#e8e8e8"/g'

index_theme "$OUT/arora-adwaita-dark" "Arora Adwaita Dark" \
    "Dark-scheme recolor of the bundled Adwaita symbolic icons"
index_theme "$OUT/arora-breeze-dark" "Arora Breeze Dark" \
    "Dark-scheme recolor of the bundled KDE Breeze icons"
index_theme "$OUT/arora-tabler-dark" "Arora Tabler Dark" \
    "Dark-scheme recolor of the bundled Tabler outline icons"

# --- the original 2009 art: final fallback ---------------------------
mkdir -p "$OUT/arora-legacy/icons" # populated via qrc aliases
cat > "$OUT/arora-legacy/index.theme" <<'EOF'
[Icon Theme]
Name=Arora Legacy
Comment=Original Arora 2009 bundled artwork — final fallback
Directories=icons
Inherits=arora-adwaita

[icons]
Size=16
Type=Scalable
MinSize=8
MaxSize=128
EOF

echo "bundled icon themes regenerated in $OUT"
