TEMPLATE = app
TARGET = tst_memcycle
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_memcycle.cpp
HEADERS += ../fakeengine.h
