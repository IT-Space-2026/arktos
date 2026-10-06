TEMPLATE = app
TARGET = tst_browserapp
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_browserapp.cpp
HEADERS +=
