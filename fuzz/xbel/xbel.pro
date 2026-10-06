TEMPLATE = app
TARGET = fuzz_xbel
QT = core gui

INCLUDEPATH += $$PWD/../../src/bookmarks $$PWD/../../src/bookmarks/xbel
DEPENDPATH += $$PWD/../../src/bookmarks $$PWD/../../src/bookmarks/xbel

include(../fuzz.pri)

HEADERS += \
    $$PWD/../../src/bookmarks/bookmarknode.h \
    $$PWD/../../src/bookmarks/xbel/xbelreader.h \
    $$PWD/../../src/bookmarks/xbel/xbelwriter.h

SOURCES += fuzz_xbel.cpp \
    $$PWD/../../src/bookmarks/bookmarknode.cpp \
    $$PWD/../../src/bookmarks/xbel/xbelreader.cpp \
    $$PWD/../../src/bookmarks/xbel/xbelwriter.cpp
