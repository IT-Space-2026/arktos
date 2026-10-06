TEMPLATE = app
TARGET = tst_webpageproxy
DEPENDPATH += .
INCLUDEPATH += .

include(../../autotests.pri)

# Input
SOURCES += tst_webpageproxy.cpp webpageproxy.cpp
HEADERS += webpageproxy.h
FORMS =
RESOURCES =
