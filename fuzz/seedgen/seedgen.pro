# Seed-corpus generator for the binary-format fuzz targets.  Not part
# of fuzz.pro (it has a main(), no libFuzzer) and not of the default
# build — build it on demand and rerun after format changes:
#     qmake6 fuzz/seedgen/seedgen.pro -o /tmp/seedgen-make && make -f ...

TEMPLATE = app
TARGET = seedgen
QT = core gui network

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/history \
    $$PWD/../../src/network/cookiejar/networkcookiejar

HEADERS += \
    $$PWD/../../src/history/historyparser.h \
    $$PWD/../../src/network/cookiejar/networkcookiejar/networkcookiejar.h

SOURCES += seedgen.cpp \
    $$PWD/../../src/history/historyparser.cpp \
    $$PWD/../../src/network/cookiejar/networkcookiejar/networkcookiejar.cpp \
    $$PWD/../../src/securestore.cpp
