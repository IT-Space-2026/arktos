TEMPLATE = app
TARGET = tst_urlcleaner
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_urlcleaner.cpp
HEADERS +=
