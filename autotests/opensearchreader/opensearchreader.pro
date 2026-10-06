TEMPLATE = app
TARGET = tst_opensearchreader
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

SOURCES += tst_opensearchreader.cpp

RESOURCES += opensearchreader.qrc
