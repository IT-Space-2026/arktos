TEMPLATE = app
TARGET = tst_qrcode
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_qrcode.cpp
HEADERS +=
