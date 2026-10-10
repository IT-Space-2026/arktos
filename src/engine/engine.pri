# ENG01 — engine-neutral interface sketch + ENG04 QtWebEngine backend.
# The QtWebEngine backend and a future Servo backend live here behind
# Engine::Backend; the app migrates onto the interface per touchpoint
# group (webView()->enginePage() is the migration entry point).

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += $$PWD/engineinterface.h \
    $$PWD/navigationpolicy.h \
    $$PWD/webenginebackend.h
SOURCES += $$PWD/engineinterface.cpp \
    $$PWD/navigationpolicy.cpp \
    $$PWD/webenginebackend.cpp
