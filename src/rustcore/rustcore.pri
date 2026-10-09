# Optional shared Rust core (RCORE01): credential custody, the
# rc_cred_* store and future engine-neutral services live in
# $$PWD (crate "arora-rustcore") behind the C ABI in include/rustcore.h.
#
# Build the crate once with
#     cd $$PWD && cargo build --release
# then configure the tree with `qmake CONFIG+=rustcore`.  The
# cargo_rustcore target below is also in PRE_TARGETDEPS, so `make`
# rebuilds the archive when the Rust sources change.  Toolchain is the
# user-local rustup (~/.cargo/bin); no sudo, no system packages.
#
# Without CONFIG+=rustcore nothing here is compiled or linked — the
# tree builds identically on machines with no Rust toolchain.
#
# Offline/CI: `cargo vendor` + a .cargo/config.toml source replacement
# works unchanged; Cargo.lock pins the resolved tree and SEC21's
# `cargo deny` policy lives in $$PWD/deny.toml.

rustcore {
    CARGO = $$system(command -v cargo || echo $$HOME/.cargo/bin/cargo)
    RUSTCORE_LIB = $$PWD/target/release/libarora_rustcore.a

    cargo_rustcore.target = $$RUSTCORE_LIB
    cargo_rustcore.commands = cd $$PWD && $$CARGO build --release
    cargo_rustcore.depends = $$PWD/Cargo.toml $$PWD/Cargo.lock \
        $$files($$PWD/src/*.rs, true)
    QMAKE_EXTRA_TARGETS += cargo_rustcore
    PRE_TARGETDEPS += $$RUSTCORE_LIB

    DEFINES += ARORA_RUSTCORE
    INCLUDEPATH += $$PWD/include
    DEPENDPATH += $$PWD/include
    HEADERS += $$PWD/include/rustcore.h \
        $$PWD/../rustcorebridge.h
    SOURCES += $$PWD/../rustcorebridge.cpp
    LIBS += $$RUSTCORE_LIB
    unix: LIBS += -ldl -lpthread -lm
}
