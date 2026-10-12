TEMPLATE = app
TARGET = tst_sandbox
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_sandbox.cpp
HEADERS +=
