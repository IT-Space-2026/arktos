TEMPLATE = app
TARGET = tst_rustdownload
DEPENDPATH += .
INCLUDEPATH += .

QT += network
include(../autotests.pri)

# Input
SOURCES += tst_rustdownload.cpp
HEADERS +=
