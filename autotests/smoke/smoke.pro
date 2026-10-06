TEMPLATE = app
TARGET = tst_smoke
DEPENDPATH += .
INCLUDEPATH += .

# Standalone toolchain smoke test: deliberately does NOT include
# ../autotests.pri (which pulls in the not-yet-ported src tree).
QT += testlib widgets webenginewidgets

mac:CONFIG -= app_bundle

RCC_DIR     = ../.rcc
UI_DIR      = ../.ui
MOC_DIR     = ../.moc
OBJECTS_DIR = ../.obj

SOURCES += tst_smoke.cpp
