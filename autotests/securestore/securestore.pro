TEMPLATE = app
TARGET = tst_securestore
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_securestore.cpp
HEADERS +=
