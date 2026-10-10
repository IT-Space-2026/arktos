# ENG01 — engine-neutral interface sketch + ENG04 QtWebEngine backend.
# The QtWebEngine backend and a future Servo backend live here behind
# Engine::Backend; the app migrates onto the interface per touchpoint
# group (webView()->enginePage() is the migration entry point).

INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

HEADERS += $$PWD/engineinterface.h \
    $$PWD/engineregistry.h \
    $$PWD/enginetab.h \
    $$PWD/navigationpolicy.h \
    $$PWD/webenginebackend.h
SOURCES += $$PWD/engineinterface.cpp \
    $$PWD/engineregistry.cpp \
    $$PWD/enginetab.cpp \
    $$PWD/navigationpolicy.cpp \
    $$PWD/webenginebackend.cpp

# ENG05: `qmake servo=1` compiles the Servo backend.  The servo-embed
# cdylib is an optional out-of-tree artifact (spikes/servo/servo-embed)
# resolved through dlopen at runtime — building with the gate on does
# NOT require the artifact, and a build without the gate is identical
# to one where the artifact is missing: Servo simply never registers.
!isEmpty(servo)|contains(CONFIG, servo) {
    DEFINES += ARORA_SERVO
    INCLUDEPATH += $$PWD/../../spikes/servo/servo-embed/include
    DEPENDPATH += $$PWD/../../spikes/servo/servo-embed/include
    HEADERS += $$PWD/servobackend.h
    SOURCES += $$PWD/servobackend.cpp
}
