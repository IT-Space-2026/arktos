# Sanitizer sweep (HARD01)

Generated: 2026-10-06 on commit b237b94 plus the HARD01 change set.
Toolchain: Ubuntu clang version 18.1.3 (1ubuntu1), qmake -spec linux-clang CONFIG+=sanitize

## Reproduce

    source .devin/qt-env.sh && make check-sanitize

Instrumented copy of the tree is built in a throwaway dir with
`-fsanitize=address,undefined`; the autotest suite and every
`--*-smoke` flag run offscreen under an isolated HOME.
LeakSanitizer is off by default (MEM01); ARORA_SANITIZE_LEAKS=1
enables it, ARORA_SANITIZE_STRICT=1 makes UBSan findings fatal.

## Result

autotest suite exit: 0 (49 test programs, all passed)
sanitizer reports: 0

No sanitizer reports.
