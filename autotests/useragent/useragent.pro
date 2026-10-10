TEMPLATE = app
TARGET = tst_useragent
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_useragent.cpp
HEADERS +=
