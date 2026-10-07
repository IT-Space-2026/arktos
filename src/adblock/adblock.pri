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

# Optional Brave adblock-rust engine (ADB02): build the C FFI
# staticlib once with `cd $$PWD/rust && cargo build --release`, then
# configure with `qmake CONFIG+=adblock_rust` to delegate matching to
# adblock::Engine.  The native C++ matcher stays the default; under
# the flag it remains compiled in for the --adblock-rust-smoke
# comparison harness.
adblock_rust {
    DEFINES += ARORA_ADBLOCK_RUST
    HEADERS += $$PWD/adblockrustengine.h
    SOURCES += $$PWD/adblockrustengine.cpp
    LIBS += $$PWD/rust/target/release/libarora_adblock_ffi.a
    LIBS += -ldl -lpthread -lm
}
