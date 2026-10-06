TEMPLATE = app
TARGET = tst_webpage
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_webpage.cpp
HEADERS +=
