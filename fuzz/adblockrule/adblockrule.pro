TEMPLATE = app
TARGET = fuzz_adblockrule
QT = core

INCLUDEPATH += $$PWD/../../src/adblock
DEPENDPATH += $$PWD/../../src/adblock

include(../fuzz.pri)

SOURCES += fuzz_adblockrule.cpp \
    $$PWD/../../src/adblock/adblockrule.cpp
