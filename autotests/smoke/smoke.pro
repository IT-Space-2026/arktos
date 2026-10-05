TEMPLATE = app
TARGET = tst_smoke
DEPENDPATH += .
INCLUDEPATH += .

# Standalone toolchain smoke test: deliberately does NOT include
# ../autotests.pri (which pulls in the not-yet-ported src tree).
QT += testlib widgets webenginewidgets

mac:CONFIG -= app_bundle

SOURCES = tst_smoke.cpp
