# Shared config for the libFuzzer targets (FUZZ01).
#
# These binaries need clang — -fsanitize=fuzzer does not exist in gcc.
# Build via .devin/run-fuzz.sh which calls qmake with the right spec:
#     qmake -spec linux-clang <target>.pro
#
# fuzzer supplies main() + coverage instrumentation; address catches
# memory errors, undefined (non-recovering) turns UB into artifacts.

!linux-clang:!macx-clang:!win32-clang-msvc:!win32-clang-g++ {
    error("fuzz targets require clang — run .devin/run-fuzz.sh")
}

QMAKE_CFLAGS   += -fsanitize=fuzzer,address,undefined \
                  -fno-sanitize-recover=undefined \
                  -fno-omit-frame-pointer -g
QMAKE_CXXFLAGS += -fsanitize=fuzzer,address,undefined \
                  -fno-sanitize-recover=undefined \
                  -fno-omit-frame-pointer -g
QMAKE_LFLAGS   += -fsanitize=fuzzer,address,undefined

CONFIG += console
mac:CONFIG -= app_bundle

RCC_DIR     = .rcc
UI_DIR      = .ui
MOC_DIR     = .moc
OBJECTS_DIR = .obj
