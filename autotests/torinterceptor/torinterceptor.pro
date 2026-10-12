TEMPLATE = app
TARGET = tst_torinterceptor
DEPENDPATH += .
INCLUDEPATH += . ../

include(../autotests.pri)

# Input
SOURCES += tst_torinterceptor.cpp
HEADERS +=

win32: CONFIG += console
mac:CONFIG -= app_bundle
