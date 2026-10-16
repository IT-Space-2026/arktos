TEMPLATE = app
TARGET = tst_vaultui
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_vaultui.cpp
HEADERS += stubvaultengine.h
