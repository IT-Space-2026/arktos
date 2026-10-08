TEMPLATE = app
TARGET = tst_httponly
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

# Input
SOURCES += tst_httponly.cpp
HEADERS +=

win32: CONFIG += console
mac:CONFIG -= app_bundle
