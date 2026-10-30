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
isEmpty(ARKTOS_SERVO_ARTIFACTS) {
    # .qmake.conf normally computes this first; repeat it so the .pri
    # still resolves correctly when parsed without the project root.
    # for() iterates a bare variable name only — a literal list or a
    # $$files() expansion loops over nothing under qmake 6.12, which is
    # why the profile dirs are named through ARKTOS_SERVO_PROFILES.
    ARKTOS_SERVO_PROFILES = debug release
    ARKTOS_SERVO_LIBNAMES = \
        libservo_embed.so libservo_embed.dylib servo_embed.dll
    for(profile, ARKTOS_SERVO_PROFILES) {
        for(libname, ARKTOS_SERVO_LIBNAMES) {
            exists($$SERVO_EMBED_DIR/target/$$profile/$$libname): \
                ARKTOS_SERVO_ARTIFACTS += \
                    $$SERVO_EMBED_DIR/target/$$profile/$$libname
        }
    }
}
ARKTOS_SERVO_ENABLED = false
contains(CONFIG, servo)|!isEmpty(servo):!equals(servo, 0):isEmpty(noservo): \
    ARKTOS_SERVO_ENABLED = true

equals(ARKTOS_SERVO_ENABLED, true) {
    DEFINES += ARKTOS_SERVO
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
    ARKTOS_BACKENDS = webengine
    equals(ARKTOS_SERVO_ENABLED, true): \
        ARKTOS_BACKENDS += servo
    message(Arktos engine backends: $$join(ARKTOS_BACKENDS, " + "))
    equals(ARKTOS_SERVO_ENABLED, true) {
        isEmpty(ARKTOS_SERVO_ARTIFACTS): \
            warning(ARKTOS_SERVO is compiled in but no servo-embed \
                    cdylib exists under spikes/servo/servo-embed/target/*/ — \
                    the backend stays unregistered until the cdylib is \
                    built (cargo build in spikes/servo/servo-embed))
        # ENG29: a debug-only artifact set means the app will dlopen an
        # unoptimized libservo — pathologically slow page loads that
        # masquerade as "the engine is just slow".  The runtime warns
        # too, but configure is where the fix belongs.  (The $$find
        # result must ride through a named variable — a nested call's
        # comma would split isEmpty()'s argument list.)
        ARKTOS_SERVO_RELEASE_ARTIFACTS = \
            $$find(ARKTOS_SERVO_ARTIFACTS, /target/release/)
        !isEmpty(ARKTOS_SERVO_ARTIFACTS): \
            isEmpty(ARKTOS_SERVO_RELEASE_ARTIFACTS): \
                warning(servo-embed artifacts exist but none is a \
                        target/release build — the app will run the \
                        unoptimized debug cdylib and pages will load \
                        pathologically slowly; build the canonical \
                        artifact with `cargo build --release` in \
                        spikes/servo/servo-embed)
    } else {
        !isEmpty(ARKTOS_SERVO_ARTIFACTS): \
            message(servo opted out — servo-embed artifact present; \
                    re-enable with qmake servo=1)
        isEmpty(ARKTOS_SERVO_ARTIFACTS): \
            message(no servo-embed artifact under \
                    spikes/servo/servo-embed/target/)
    }
}
