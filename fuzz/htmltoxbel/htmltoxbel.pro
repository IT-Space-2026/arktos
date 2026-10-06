TEMPLATE = app
TARGET = fuzz_htmltoxbel
QT = core gui

INCLUDEPATH += $$PWD/../../tools/htmlToXBel
DEPENDPATH += $$PWD/../../tools/htmlToXBel

include(../fuzz.pri)

HEADERS += $$PWD/../../tools/htmlToXBel/converter.h

SOURCES += fuzz_htmltoxbel.cpp \
    $$PWD/../../tools/htmlToXBel/converter.cpp
