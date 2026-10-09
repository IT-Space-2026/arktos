# ENG01 — engine-neutral interface sketch (compiled, unused).
# The QtWebEngine backend and a future Servo backend will live here
# behind Engine::Backend; today only the boundary types compile.

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += $$PWD/engineinterface.h
SOURCES += $$PWD/engineinterface.cpp
