# Sanitizer sweep (HARD01)

Generated: 2026-10-14 21:51 UTC on commit 07b85c4
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
sanitizer reports: 52

```
ubsan.4136185: ==4136185==ERROR: AddressSanitizer: SEGV on unknown address 0x001800003a28 (pc 0x79d69d848806 bp 0x79d6813d8a30 sp 0x79d6813d8a00 T11)
ubsan.4136185: SUMMARY: AddressSanitizer: SEGV stdlib/getenv.c:31:10 in getenv
ubsan.4136576: ==4136576==ERROR: AddressSanitizer: heap-use-after-free on address 0x511000008c50 at pc 0x621248123a1f bp 0x7fff70e78bb0 sp 0x7fff70e78368
ubsan.4136576: SUMMARY: AddressSanitizer: heap-use-after-free (/tmp/arora-san.koyjxK/autotests/clearplan/tst_clearplan+0xa2da1e) (BuildId: e92cddb1744229c133c8c393f4af968986170348) in memcpy
ubsan.4142450: /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14: runtime error: downcast of address 0x5190005c5880 which does not point to an object of type 'BrowserMainWindow'
ubsan.4142450: ../../src/extensions/extensioneventhub.cpp:417:21: runtime error: downcast of address 0x5110004fb680 which does not point to an object of type 'WebView'
ubsan.4142450: ../../src/extensions/extensioneventhub.cpp:437:9: runtime error: downcast of address 0x5190005c5880 which does not point to an object of type 'BrowserMainWindow'
ubsan.4142450: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14 
ubsan.4142450: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:417:21 
ubsan.4142450: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:437:9 
ubsan.4146439: ../../src/downloadmanager.cpp:1749:56: runtime error: inf is outside the range of representable values of type 'int'
ubsan.4146439: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/downloadmanager.cpp:1749:56 
ubsan.4149947: /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14: runtime error: downcast of address 0x519000242c80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4149947: ../../src/browsermainwindow.cpp:3606:12: runtime error: member access within address 0x519000242c80 which does not point to an object of type 'const BrowserMainWindow'
ubsan.4149947: ../../src/extensions/extensioneventhub.cpp:417:21: runtime error: downcast of address 0x511000035100 which does not point to an object of type 'WebView'
ubsan.4149947: ../../src/extensions/extensioneventhub.cpp:437:9: runtime error: downcast of address 0x519000242c80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4149947: ../../src/extensions/extensioneventhub.cpp:468:33: runtime error: member call on address 0x519000242c80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4149947: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14 
ubsan.4149947: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/browsermainwindow.cpp:3606:12 
ubsan.4149947: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:417:21 
ubsan.4149947: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:437:9 
ubsan.4149947: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:468:33 
ubsan.4150299: /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14: runtime error: downcast of address 0x5190002ae480 which does not point to an object of type 'BrowserMainWindow'
ubsan.4150299: ../../src/browsermainwindow.cpp:3606:12: runtime error: member access within address 0x51900053de80 which does not point to an object of type 'const BrowserMainWindow'
ubsan.4150299: ../../src/extensions/extensioneventhub.cpp:417:21: runtime error: downcast of address 0x511000408c00 which does not point to an object of type 'WebView'
ubsan.4150299: ../../src/extensions/extensioneventhub.cpp:437:9: runtime error: downcast of address 0x5190002ae480 which does not point to an object of type 'BrowserMainWindow'
ubsan.4150299: ../../src/extensions/extensioneventhub.cpp:468:33: runtime error: member call on address 0x51900053de80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4150299: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14 
ubsan.4150299: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/browsermainwindow.cpp:3606:12 
ubsan.4150299: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:417:21 
ubsan.4150299: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:437:9 
ubsan.4150299: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:468:33 
ubsan.4156911: /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14: runtime error: downcast of address 0x519000374b80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4156911: ../../src/browsermainwindow.cpp:3606:12: runtime error: member access within address 0x51900046eb80 which does not point to an object of type 'const BrowserMainWindow'
ubsan.4156911: ../../src/browsermainwindow.cpp:641:14: runtime error: member access within address 0x51900018e780 which does not point to an object of type 'BrowserMainWindow'
ubsan.4156911: ../../src/extensions/extensioneventhub.cpp:417:21: runtime error: downcast of address 0x5110001434c0 which does not point to an object of type 'WebView'
ubsan.4156911: ../../src/extensions/extensioneventhub.cpp:437:9: runtime error: downcast of address 0x5190002e6d80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4156911: ../../src/extensions/extensioneventhub.cpp:468:33: runtime error: member call on address 0x51900046eb80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14 
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/browsermainwindow.cpp:3606:12 
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/browsermainwindow.cpp:641:14 
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:417:21 
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:437:9 
ubsan.4156911: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior ../../src/extensions/extensioneventhub.cpp:468:33 
ubsan.4159246: browsermainwindow.cpp:641:14: runtime error: member access within address 0x519000271a80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4159246: extensions/extensioneventhub.cpp:417:21: runtime error: downcast of address 0x51100016ea80 which does not point to an object of type 'WebView'
ubsan.4159246: extensions/extensioneventhub.cpp:437:9: runtime error: downcast of address 0x519000271a80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4159246: /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14: runtime error: downcast of address 0x519000271a80 which does not point to an object of type 'BrowserMainWindow'
ubsan.4159246: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior browsermainwindow.cpp:641:14 
ubsan.4159246: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior extensions/extensioneventhub.cpp:417:21 
ubsan.4159246: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior extensions/extensioneventhub.cpp:437:9 
ubsan.4159246: SUMMARY: UndefinedBehaviorSanitizer: undefined-behavior /home/szefi/Qt/6.12.0/gcc_64/include/QtCore/qpointer.h:76:14 
```
