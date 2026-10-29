# MB05 — the middleware bridge harness: compiles the freestanding
# rustcore probe and runs its --bridge leg (one query+command per
# envelope domain, a namespaced-event round trip, and the engine
# capability matrix) as a suite binary.  The probe needs no Qt — the
# point is that the C ABI alone is the bridge's consumer surface.
#
# rustcore-gated: a no-rust build has no static library to link, so
# the skip stub compiles instead and the suite still runs green.

TEMPLATE = app
TARGET = tst_acprobe
CONFIG -= qt app_bundle
CONFIG += console

# Same shared object dir as the Qt autotests (autotests.pri) so the
# dir stays clean of .o files.
OBJECTS_DIR = $$PWD/../.obj

rustcore:!no-rust {
    CARGO = $$ARKTOS_CARGO
    isEmpty(CARGO): CARGO = cargo
    win32: RUSTCORE_LIB = $$PWD/../../src/rustcore/target/release/arktos_rustcore.lib
    else:  RUSTCORE_LIB = $$PWD/../../src/rustcore/target/release/libarktos_rustcore.a

    # Same rebuild contract as src/rustcore/rustcore.pri — the
    # archive tracks the Rust sources, scrubbing flag env vars a
    # parent build may export into the recipe.
    cargo_rustcore.target = $$RUSTCORE_LIB
    unix: cargo_rustcore.commands = cd $$PWD/../../src/rustcore && \
        env -u CFLAGS -u CXXFLAGS -u CPPFLAGS -u LDFLAGS \
        MACOSX_DEPLOYMENT_TARGET=$$QMAKE_MACOSX_DEPLOYMENT_TARGET \
        $$CARGO build --release
    win32: cargo_rustcore.commands = cd $$PWD/../../src/rustcore && \
        $$CARGO build --release
    cargo_rustcore.depends = $$PWD/../../src/rustcore/Cargo.toml \
        $$PWD/../../src/rustcore/Cargo.lock \
        $$files($$PWD/../../src/rustcore/src/*.rs, true)
    QMAKE_EXTRA_TARGETS += cargo_rustcore
    PRE_TARGETDEPS += $$RUSTCORE_LIB

    DEFINES += ACPROBE_BRIDGE_DEFAULT
    INCLUDEPATH += $$PWD/../../src/rustcore/include
    DEPENDPATH += $$PWD/../../src/rustcore/include
    SOURCES += $$PWD/../../src/rustcore/tools/acprobe.c
    LIBS += $$RUSTCORE_LIB
    unix: LIBS += -ldl -lpthread -lm
    win32: LIBS += -ladvapi32 -lbcrypt -lntdll -luserenv -lws2_32
    # IOKit: same sysinfo/objc2_io_kit surface as rustcore.pri —
    # tst_acprobe links the archive directly.
    macx: LIBS += -framework Security -framework CoreFoundation -framework IOKit
} else {
    SOURCES += acprobe_norust.c
}
