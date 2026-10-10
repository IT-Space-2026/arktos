TEMPLATE = app
TARGET = tst_crashreporter
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_crashreporter.cpp
HEADERS +=
