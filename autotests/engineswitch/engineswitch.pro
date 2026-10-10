TEMPLATE = app
TARGET = tst_engineswitch
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

# Input
SOURCES += tst_engineswitch.cpp
HEADERS += ../fakeengine.h
