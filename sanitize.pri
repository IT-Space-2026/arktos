# HARD01: AddressSanitizer + UndefinedBehaviorSanitizer build variant.
# Build an instrumented copy of the tree with:
#     qmake -spec linux-clang CONFIG+=sanitize
# (linux-g++ also works — gcc 13 understands the same flags — but clang
# produces better UBSan diagnostics and matches the fuzz/coverage
# toolchains).  .devin/check-sanitize.sh automates build+run+triage.
#
# CONFIG+=sanitize-strict additionally turns every UBSan check fatal
# (-fno-sanitize-recover=all) — used by the script's second pass once
# the recoverable findings are triaged.
sanitize {
    QMAKE_CFLAGS   += -fsanitize=address,undefined -fno-omit-frame-pointer -g
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-omit-frame-pointer -g
    QMAKE_LFLAGS   += -fsanitize=address,undefined
}
sanitize-strict {
    QMAKE_CFLAGS   += -fsanitize=address,undefined -fno-sanitize-recover=all \
                      -fno-omit-frame-pointer -g
    QMAKE_CXXFLAGS += -fsanitize=address,undefined -fno-sanitize-recover=all \
                      -fno-omit-frame-pointer -g
    QMAKE_LFLAGS   += -fsanitize=address,undefined
}
