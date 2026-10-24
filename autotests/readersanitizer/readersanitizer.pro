TEMPLATE = app
TARGET = tst_readersanitizer
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

# Input
SOURCES += tst_readersanitizer.cpp
RESOURCES += readersanitizer.qrc
