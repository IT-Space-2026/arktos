TEMPLATE = app
TARGET = tst_opensearchwriter
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

SOURCES += tst_opensearchwriter.cpp

RESOURCES += opensearchwriter.qrc
