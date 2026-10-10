INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += \
    $$PWD/adblockdialog.h \
    $$PWD/adblockmanager.h \
    $$PWD/adblocknetwork.h \
    $$PWD/adblockpage.h \
    $$PWD/adblockpresets.h \
    $$PWD/adblockpresetsdialog.h \
    $$PWD/adblockrequestinterceptor.h \
    $$PWD/adblockresourcehandler.h \
    $$PWD/adblockrule.h \
    $$PWD/adblockschemeaccesshandler.h \
    $$PWD/adblocksubscription.h

SOURCES += \
    $$PWD/adblockdialog.cpp \
    $$PWD/adblockmanager.cpp \
    $$PWD/adblocknetwork.cpp \
    $$PWD/adblockpage.cpp \
    $$PWD/adblockpresets.cpp \
    $$PWD/adblockpresetsdialog.cpp \
    $$PWD/adblockrequestinterceptor.cpp \
    $$PWD/adblockresourcehandler.cpp \
    $$PWD/adblockrule.cpp \
    $$PWD/adblockschemeaccesshandler.cpp \
    $$PWD/adblocksubscription.cpp

FORMS += \
    $$PWD/adblockdialog.ui

# Brave adblock-rust engine (ADB02, default-on per RDEF01): the flag
# is set by default in the project-root .qmake.conf whenever a cargo toolchain
# resolves, and matching delegates to adblock::Engine behind the C
# FFI staticlib in $$PWD/rust.  The native C++ matcher remains
# compiled in for the --adblock-rust-smoke comparison harness and is
# the fallback when the crate is opted out (`qmake
# CONFIG-=adblock_rust`, or `CONFIG+=no-rust` for all crates) or no
# toolchain exists.
adblock_rust:!no-rust {
    CARGO = $$system(command -v cargo || echo $$(HOME)/.cargo/bin/cargo)
    ADBLOCK_RUST_LIB = $$PWD/rust/target/release/libarora_adblock_ffi.a

    cargo_adblock_rust.target = $$ADBLOCK_RUST_LIB
    cargo_adblock_rust.commands = cd $$PWD/rust && $$CARGO build --release
    cargo_adblock_rust.depends = $$PWD/rust/Cargo.toml \
        $$PWD/rust/Cargo.lock $$files($$PWD/rust/src/*.rs, true)
    QMAKE_EXTRA_TARGETS += cargo_adblock_rust
    PRE_TARGETDEPS += $$ADBLOCK_RUST_LIB

    DEFINES += ARORA_ADBLOCK_RUST
    HEADERS += $$PWD/adblockrustengine.h
    SOURCES += $$PWD/adblockrustengine.cpp
    LIBS += $$ADBLOCK_RUST_LIB
    LIBS += -ldl -lpthread -lm
}
