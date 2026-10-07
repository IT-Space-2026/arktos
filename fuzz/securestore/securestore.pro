TEMPLATE = app
TARGET = fuzz_securestore
QT = core

INCLUDEPATH += $$PWD/../../src
DEPENDPATH += $$PWD/../../src

include(../fuzz.pri)

HEADERS += $$PWD/../../src/securestore.h \
    $$PWD/../../src/argon2id.h

SOURCES += fuzz_securestore.cpp \
    $$PWD/../../src/securestore.cpp \
    $$PWD/../../src/argon2id.c
