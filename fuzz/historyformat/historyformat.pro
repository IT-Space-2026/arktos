TEMPLATE = app
TARGET = fuzz_historyformat
QT = core gui

INCLUDEPATH += $$PWD/../../src $$PWD/../../src/history
DEPENDPATH += $$PWD/../../src/history

include(../fuzz.pri)

HEADERS += \
    $$PWD/../../src/history/historyparser.h

SOURCES += fuzz_historyformat.cpp \
    $$PWD/../../src/history/historyparser.cpp
