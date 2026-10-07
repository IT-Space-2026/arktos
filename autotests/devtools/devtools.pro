TEMPLATE = app
TARGET = tst_devtools
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_devtools.cpp
HEADERS +=
