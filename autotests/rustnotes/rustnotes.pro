TEMPLATE = app
TARGET = tst_rustnotes
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_rustnotes.cpp
HEADERS +=
