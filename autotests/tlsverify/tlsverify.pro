TEMPLATE = app
TARGET = tst_tlsverify
DEPENDPATH += .
INCLUDEPATH += .

QT += network

include(../autotests.pri)

# Input
SOURCES += tst_tlsverify.cpp
HEADERS +=
