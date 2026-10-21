TEMPLATE = app
TARGET = tst_rustprofiles
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_rustprofiles.cpp
HEADERS +=
