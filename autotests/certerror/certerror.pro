TEMPLATE = app
TARGET = tst_certerror
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_certerror.cpp
HEADERS +=
win32: CONFIG += console
mac:CONFIG -= app_bundle
