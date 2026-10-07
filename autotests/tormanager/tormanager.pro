TEMPLATE = app
TARGET = tst_tormanager
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_tormanager.cpp
HEADERS +=
DISTFILES += faketor.py
win32: CONFIG += console
mac:CONFIG -= app_bundle

QT += testlib
