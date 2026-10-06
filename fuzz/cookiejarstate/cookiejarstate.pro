TEMPLATE = app
TARGET = fuzz_cookiejarstate
QT = core network

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/network/cookiejar/networkcookiejar
DEPENDPATH += $$PWD/../../src/network/cookiejar/networkcookiejar

include(../fuzz.pri)

HEADERS += \
    $$PWD/../../src/network/cookiejar/networkcookiejar/networkcookiejar.h \
    $$PWD/../../src/network/cookiejar/networkcookiejar/networkcookiejar_p.h \
    $$PWD/../../src/network/cookiejar/networkcookiejar/trie_p.h \
    $$PWD/../../src/network/cookiejar/networkcookiejar/twoleveldomains_p.h

SOURCES += fuzz_cookiejarstate.cpp \
    $$PWD/../../src/network/cookiejar/networkcookiejar/networkcookiejar.cpp
