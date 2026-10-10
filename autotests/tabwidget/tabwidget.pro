TEMPLATE = app
TARGET = tst_tabwidget
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_tabwidget.cpp
HEADERS += ../fakeengine.h
