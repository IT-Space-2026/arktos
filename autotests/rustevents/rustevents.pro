TEMPLATE = app
TARGET = tst_rustevents
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_rustevents.cpp
HEADERS +=
