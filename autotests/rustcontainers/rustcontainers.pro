TEMPLATE = app
TARGET = tst_rustcontainers
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_rustcontainers.cpp
HEADERS +=
