TEMPLATE = app
TARGET = tst_singleapplication
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_singleapplication.cpp
HEADERS +=
