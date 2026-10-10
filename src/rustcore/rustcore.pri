# Shared Rust core (RCORE01, default-on per RDEF01): credential
# custody, the rc_cred_* store and future engine-neutral services
# live in $$PWD (crate "arora-rustcore") behind the C ABI in
# include/rustcore.h.
#
# The flag is set by default in the project-root .qmake.conf whenever a cargo
# toolchain resolves; `make` then rebuilds the archive via the
# cargo_rustcore target below when the Rust sources change.
# Toolchain is the user-local rustup (~/.cargo/bin); no sudo, no
# system packages.
#
# Opt out with `qmake CONFIG-=rustcore` (this crate only) or
# `qmake CONFIG+=no-rust` (all crates) — nothing here is compiled or
# linked and the Qt fallback path builds instead.
#
# Offline/CI: `cargo vendor` + a .cargo/config.toml source replacement
# works unchanged; Cargo.lock pins the resolved tree and SEC21's
# `cargo deny` policy lives in $$PWD/deny.toml.

rustcore:!no-rust {
    CARGO = $$system(command -v cargo || echo $$(HOME)/.cargo/bin/cargo)
    RUSTCORE_LIB = $$PWD/target/release/libarora_rustcore.a

    cargo_rustcore.target = $$RUSTCORE_LIB
    cargo_rustcore.commands = cd $$PWD && $$CARGO build --release
    cargo_rustcore.depends = $$PWD/Cargo.toml $$PWD/Cargo.lock \
        $$files($$PWD/src/*.rs, true) $$files($$PWD/data/*)
    QMAKE_EXTRA_TARGETS += cargo_rustcore
    PRE_TARGETDEPS += $$RUSTCORE_LIB

    DEFINES += ARORA_RUSTCORE
    INCLUDEPATH += $$PWD/include
    DEPENDPATH += $$PWD/include
    HEADERS += $$PWD/include/rustcore.h \
        $$PWD/../rustcorebridge.h \
        $$PWD/../sitedecisionstore.h
    SOURCES += $$PWD/../rustcorebridge.cpp \
        $$PWD/../sitedecisionstore.cpp
    LIBS += $$RUSTCORE_LIB
    unix: LIBS += -ldl -lpthread -lm
}
