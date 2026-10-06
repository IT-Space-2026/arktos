TEMPLATE = app
TARGET = fuzz_streamingutils
QT = core network

INCLUDEPATH += $$PWD/../../src \
    $$PWD/../../src/network/cookiejar/networkcookiejar
DEPENDPATH += $$PWD/../../src

include(../fuzz.pri)

SOURCES += fuzz_streamingutils.cpp
