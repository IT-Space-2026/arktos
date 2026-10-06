TEMPLATE = app
TARGET = tst_privatebrowsing
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_privatebrowsing.cpp
HEADERS +=
