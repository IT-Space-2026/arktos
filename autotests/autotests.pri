win32: CONFIG += console
mac:CONFIG -= app_bundle

QT += testlib

include($$PWD/../src/src.pri)
include($$PWD/modeltest/modeltest.pri)

HEADERS += $$PWD/qtest_arora.h

DEFINES += AUTOTESTS

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

RCC_DIR     = $$PWD/.rcc
UI_DIR      = $$PWD/.ui
MOC_DIR     = $$PWD/.moc
OBJECTS_DIR = $$PWD/.obj


