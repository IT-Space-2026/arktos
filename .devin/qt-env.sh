# Source this before building Arora with the user-local Qt 6.
#   source .devin/qt-env.sh && qmake && make -j2
# A plain qmake builds the Rust components by default (RDEF01 — the
# root .qmake.conf auto-detects cargo); pass CONFIG+=no-rust or
# CONFIG-=rustcore/-rustdl/-adblock_rust for the Qt fallback paths.
export QTDIR="$HOME/Qt/6.12.0/gcc_64"
export PATH="$QTDIR/bin:$PATH"
export LD_LIBRARY_PATH="$QTDIR/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export QT_PLUGIN_PATH="$QTDIR/plugins"
export QT_QPA_PLATFORM=offscreen   # default: headless, never touch the live display
