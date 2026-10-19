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
    for(dir, $$files($$SERVO_EMBED_DIR/target/*)) {
        exists($$dir/libservo_embed.so): \
            ARKTOS_SERVO_ARTIFACTS += $$dir/libservo_embed.so
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

# WK06: the WPE WebKit backend gate mirrors the servo stanza — the
# auto-enable lives in .qmake.conf (pkg-config resolves wpe-webkit-2.0
# + wpe-platform-2.0 once .devin/webkit-env.sh is sourced, or a system
# WPE >= 2.54 install supplies them); explicit opt-outs always win:
# `qmake CONFIG-=webkit`, `nowebkit=1` or `webkit=0`.
equals(webkit, 0)|!isEmpty(nowebkit)|contains(CONFIG, nowebkit): \
    CONFIG -= webkit

isEmpty(ARKTOS_WEBKIT_PKGCONFIG) {
    # .qmake.conf normally probes first; repeat it so the .pri still
    # resolves correctly when parsed without the project root.
    system(pkg-config --exists wpe-webkit-2.0 wpe-platform-2.0): \
        ARKTOS_WEBKIT_PKGCONFIG = true
}
ARKTOS_WEBKIT_ENABLED = false
contains(CONFIG, webkit)|!isEmpty(webkit):!equals(webkit, 0):isEmpty(nowebkit): \
    ARKTOS_WEBKIT_ENABLED = true

equals(ARKTOS_WEBKIT_ENABLED, true) {
    # Compile-but-inert like servobackend: the TU is in the build
    # whenever the backend is enabled — a `qmake webkit=1` build on a
    # host whose pkg-config cannot resolve the WPE packages still
    # compiles webkitbackend.cpp, and the backend simply reports the
    # engine unavailable at runtime (initialize() false,
    # pageUnavailableReason() set).  ARKTOS_WEBKIT_LINKED marks the
    # builds where the libraries actually resolve and are linked.
    DEFINES += ARKTOS_WEBKIT
    HEADERS += $$PWD/webkitbackend.h
    SOURCES += $$PWD/webkitbackend.cpp
    equals(ARKTOS_WEBKIT_PKGCONFIG, true) {
        DEFINES += ARKTOS_WEBKIT_LINKED
        CONFIG += link_pkgconfig
        PKGCONFIG += wpe-webkit-2.0 wpe-platform-2.0
        # The ~/wpe sideloaded-glibc recipe (.devin/webkit-env.sh +
        # ~/wpe/ACQUIRED.md): every binary carrying the backend TU —
        # app, tools, autotests — links libWPEWebKit, so every binary
        # needs the sid ld.so interp + transitive DT_RPATH + the
        # g_sort_array compat shim + explicit sid libc/libm for the
        # GLIBC_2.4x versioned refs.  Applied only when the env
        # contract is present — a system WPE on a new-enough host
        # glibc links plain.  `webkit=0` builds stay clean of all of
        # it.
        unix:!mac {
            ARKTOS_WPE_LIBDIR_Q = $$(ARKTOS_WPE_LIBDIR)
            ARKTOS_WPE_GLIBC_Q = $$(ARKTOS_WPE_GLIBC)
            ARKTOS_WPE_DYNAMIC_LINKER_Q = $$(ARKTOS_WPE_DYNAMIC_LINKER)
            ARKTOS_WPE_COMPAT_Q = $$(ARKTOS_WPE_COMPAT)
            !isEmpty(ARKTOS_WPE_DYNAMIC_LINKER_Q): \
            !isEmpty(ARKTOS_WPE_LIBDIR_Q): \
            !isEmpty(ARKTOS_WPE_GLIBC_Q) {
                LIBS += -L$$ARKTOS_WPE_LIBDIR_Q
                !isEmpty(ARKTOS_WPE_COMPAT_Q): \
                    LIBS += -Wl,--no-as-needed $$ARKTOS_WPE_COMPAT_Q \
                            -Wl,--as-needed
                LIBS += $$ARKTOS_WPE_GLIBC_Q/libc.so.6 \
                        $$ARKTOS_WPE_GLIBC_Q/libm.so.6
                QMAKE_LFLAGS += \
                    -Wl,--dynamic-linker,$$ARKTOS_WPE_DYNAMIC_LINKER_Q \
                    -Wl,--disable-new-dtags
                QMAKE_RPATHDIR += $$ARKTOS_WPE_LIBDIR_Q \
                                  $$ARKTOS_WPE_GLIBC_Q
            }
        }
    }
}

# ENG10: announce the resolved backend set once per qmake run (the .pri
# is included from many sub-projects which do not share variable scope,
# so gate on the consuming .pro being src.pro itself) — a dropped engine
# can never go unnoticed again.
equals(_PRO_FILE_, $$clean_path($$PWD/../src.pro)) {
    ARKTOS_BACKENDS = webengine
    equals(ARKTOS_SERVO_ENABLED, true): \
        ARKTOS_BACKENDS += servo
    equals(ARKTOS_WEBKIT_ENABLED, true): \
        ARKTOS_BACKENDS += webkit
    message(Arktos engine backends: $$join(ARKTOS_BACKENDS, " + "))
    equals(ARKTOS_SERVO_ENABLED, true) {
        isEmpty(ARKTOS_SERVO_ARTIFACTS): \
            warning(ARKTOS_SERVO is compiled in but no libservo_embed.so \
                    exists under spikes/servo/servo-embed/target/*/ — \
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
    equals(ARKTOS_WEBKIT_ENABLED, true) {
        !equals(ARKTOS_WEBKIT_PKGCONFIG, true): \
            warning(ARKTOS_WEBKIT is compiled in but pkg-config cannot \
                    resolve wpe-webkit-2.0 + wpe-platform-2.0 — the \
                    backend compiles inert and reports unavailable \
                    until the WPE stack is installed (source \
                    .devin/webkit-env.sh for the user-local ~/wpe \
                    prefix))
    } else {
        equals(ARKTOS_WEBKIT_PKGCONFIG, true): \
            message(webkit opted out — WPE WebKit packages resolvable; \
                    re-enable with qmake webkit=1)
        !equals(ARKTOS_WEBKIT_PKGCONFIG, true): \
            message(no WPE WebKit pkg-config packages — the webkit \
                    backend is out of this build (source \
                    .devin/webkit-env.sh for the ~/wpe stack, or \
                    qmake webkit=1 to force an inert backend))
    }
}
