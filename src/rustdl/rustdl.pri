# Accelerated download engine (DLACC03/DLACC06, default-on per
# RDEF01): the segmented HTTP(S) fetcher lives in $$PWD (crate
# "arora-rustdl") behind the C ABI in include/rustdl.h, consumed by
# src/rustdownloadengine.{h,cpp} which maps a handle onto the
# existing download-manager card UI.
#
# The flag is set by default in the project-root .qmake.conf whenever a cargo
# toolchain resolves; `make` then rebuilds the archive via the
# cargo_rustdl target below when the Rust sources change.
# Toolchain is the user-local rustup (~/.cargo/bin); no sudo, no
# system packages.
#
# Opt out with `qmake CONFIG-=rustdl` (this crate only) or
# `qmake CONFIG+=no-rust` (all crates) — nothing here is compiled or
# linked, the built-in download engine stays the only backend, and the
# download-engine selector (DLACC01) shows "Accelerated" disabled.
#
# Offline/CI: `cargo vendor` + a .cargo/config.toml source replacement
# works unchanged; Cargo.lock pins the resolved tree and SEC21's
# `cargo deny` policy lives in $$PWD/deny.toml.

rustdl:!no-rust {
    CARGO = $$system(command -v cargo || echo $$(HOME)/.cargo/bin/cargo)
    RUSTDL_LIB = $$PWD/target/release/libarora_rustdl.a

    cargo_rustdl.target = $$RUSTDL_LIB
    cargo_rustdl.commands = cd $$PWD && $$CARGO build --release
    cargo_rustdl.depends = $$PWD/Cargo.toml $$PWD/Cargo.lock \
        $$files($$PWD/src/*.rs, true)
    QMAKE_EXTRA_TARGETS += cargo_rustdl
    PRE_TARGETDEPS += $$RUSTDL_LIB

    DEFINES += ARORA_RUSTDL
    INCLUDEPATH += $$PWD/include
    DEPENDPATH += $$PWD/include
    HEADERS += $$PWD/include/rustdl.h \
        $$PWD/../rustdownloadengine.h \
        $$PWD/../downloadworker.h
    SOURCES += $$PWD/../rustdownloadengine.cpp \
        $$PWD/../downloadworker.cpp
    LIBS += $$RUSTDL_LIB
    unix: LIBS += -ldl -lpthread -lm
}
