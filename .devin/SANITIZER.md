# Sanitizer sweep (HARD01)

Generated: 2026-10-10 09:22 UTC on commit ab897ba
Toolchain: Ubuntu clang version 18.1.3 (1ubuntu1), qmake -spec linux-clang CONFIG+=sanitize

## Reproduce

    source .devin/qt-env.sh && make check-sanitize

Instrumented copy of the tree is built in a throwaway dir with
`-fsanitize=address,undefined`; the autotest suite and every
`--*-smoke` flag run offscreen under an isolated HOME.
LeakSanitizer is off by default (MEM01); ARORA_SANITIZE_LEAKS=1
enables it, ARORA_SANITIZE_STRICT=1 makes UBSan findings fatal.

## Result

autotest suite exit: 1
sanitizer reports: 10

```
ubsan.350891: ==350891==ERROR: AddressSanitizer: SEGV on unknown address 0x0029000076de (pc 0x7fe8b3248806 bp 0x7fe87a5d6ce0 sp 0x7fe87a5d6cb0 T30)
ubsan.350891: SUMMARY: AddressSanitizer: SEGV stdlib/getenv.c:31:10 in getenv
ubsan.352151: ==352151==ERROR: AddressSanitizer: SEGV on unknown address 0x002900007646 (pc 0x78e954248806 bp 0x78e91b7d6cd0 sp 0x78e91b7d6ca0 T30)
ubsan.352151: SUMMARY: AddressSanitizer: SEGV stdlib/getenv.c:31:10 in getenv
ubsan.354285: ==354285==ERROR: AddressSanitizer: heap-use-after-free on address 0x50f000004890 at pc 0x6168901376cc bp 0x7ffcf9fd0d30 sp 0x7ffcf9fd0d28
ubsan.354285: SUMMARY: AddressSanitizer: heap-use-after-free /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qarraydatapointer.h:112:45 in QArrayDataPointer<char16_t>::data() const
ubsan.357859: ==357859==ERROR: AddressSanitizer: SEGV on unknown address 0x00290000d58e (pc 0x700ecec48806 bp 0x700e90da96d0 sp 0x700e90da96a0 T34)
ubsan.357859: SUMMARY: AddressSanitizer: SEGV stdlib/getenv.c:31:10 in getenv
ubsan.359839: ==359839==ERROR: AddressSanitizer: SEGV on unknown address 0x00290000d59e (pc 0x74d629e48806 bp 0x74d5f0dd6cd0 sp 0x74d5f0dd6ca0 T30)
ubsan.359839: SUMMARY: AddressSanitizer: SEGV stdlib/getenv.c:31:10 in getenv
```
