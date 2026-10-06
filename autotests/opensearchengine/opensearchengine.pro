TEMPLATE = app
TARGET = tst_opensearchengine
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

SOURCES += tst_opensearchengine.cpp

RESOURCES += opensearchengine.qrc
