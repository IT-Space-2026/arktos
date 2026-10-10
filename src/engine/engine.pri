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

# ENG05/ENG10: the Servo backend compiles when CONFIG+=servo reaches
# this file — the root .qmake.conf auto-adds it while a servo-embed
# cdylib exists under spikes/servo/servo-embed/target/*/ so a plain
# `qmake && make` can no longer silently drop the backend.  The
# artifact is resolved through dlopen at runtime, so a servo-enabled
# build never links it; `qmake servo=1` still opts in when the
# artifact is absent (the backend compiles, Servo simply never
# registers until the cdylib is built).  Explicit opt-outs always win:
# `qmake CONFIG-=servo`, `noservo=1` or `servo=0`.
equals(servo, 0)|!isEmpty(noservo)|contains(CONFIG, noservo): \
    CONFIG -= servo

SERVO_EMBED_DIR = $$PWD/../../spikes/servo/servo-embed
isEmpty(ARORA_SERVO_ARTIFACTS) {
    # .qmake.conf normally computes this first; repeat it so the .pri
    # still resolves correctly when parsed without the project root.
    for(dir, $$files($$SERVO_EMBED_DIR/target/*)) {
        exists($$dir/libservo_embed.so): \
            ARORA_SERVO_ARTIFACTS += $$dir/libservo_embed.so
    }
}
ARORA_SERVO_ENABLED = false
contains(CONFIG, servo)|!isEmpty(servo):!equals(servo, 0):isEmpty(noservo): \
    ARORA_SERVO_ENABLED = true

equals(ARORA_SERVO_ENABLED, true) {
    DEFINES += ARORA_SERVO
    INCLUDEPATH += $$SERVO_EMBED_DIR/include
    DEPENDPATH += $$SERVO_EMBED_DIR/include
    HEADERS += $$PWD/servobackend.h
    SOURCES += $$PWD/servobackend.cpp
}

# ENG10: announce the resolved backend set once per qmake run (the .pri
# is included from many sub-projects which do not share variable scope,
# so gate on the consuming .pro being src.pro itself) — a dropped engine
# can never go unnoticed again.
equals(_PRO_FILE_, $$clean_path($$PWD/../src.pro)) {
    equals(ARORA_SERVO_ENABLED, true) {
        message(Arora engine backends: webengine + servo)
        isEmpty(ARORA_SERVO_ARTIFACTS): \
            warning(ARORA_SERVO is compiled in but no libservo_embed.so \
                    exists under spikes/servo/servo-embed/target/*/ — \
                    the backend stays unregistered until the cdylib is \
                    built (cargo build in spikes/servo/servo-embed))
    } else {
        !isEmpty(ARORA_SERVO_ARTIFACTS): \
            message(Arora engine backends: webengine only — servo opted \
                    out (servo-embed artifact present; re-enable with \
                    qmake servo=1))
        isEmpty(ARORA_SERVO_ARTIFACTS): \
            message(Arora engine backends: webengine only — no \
                    servo-embed artifact under \
                    spikes/servo/servo-embed/target/)
    }
}
