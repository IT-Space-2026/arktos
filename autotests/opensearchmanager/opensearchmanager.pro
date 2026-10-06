TEMPLATE = app
TARGET = tst_opensearchmanager
DEPENDPATH += .
INCLUDEPATH += .

include(../autotests.pri)

SOURCES += \
    tst_opensearchmanager.cpp