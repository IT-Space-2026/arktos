# CEF01 spike probe — NOT in the top-level SUBDIRS build.
# Requires: source .devin/qt-env.sh; CEF_ROOT points at an extracted
# cef_binary_*_linux64 standard distribution with libcef_dll_wrapper
# already built (build/libcef_dll_wrapper/libcef_dll_wrapper.a) —
# see spikes/cef/README.md for the acquisition + cmake steps.
#   cd spikes/cef/qcefprobe && ./build-probe.sh && ./run-render.sh
TEMPLATE = app
TARGET = qcefprobe
QT += widgets
# CEF >= M139 headers are C++20 (std::same_as/std::derived_from in
# cef_scoped_refptr.h) — c++17 no longer compiles the distro includes.
CONFIG += c++2a warn_on

SOURCES += qcefprobe.cpp

CEF_ROOT = $$(CEF_ROOT)
isEmpty(CEF_ROOT): error("CEF_ROOT env missing — see spikes/cef/README.md")
!exists($$CEF_ROOT/include/cef_app.h): error("CEF_ROOT has no include/cef_app.h")
!exists($$CEF_ROOT/build/libcef_dll_wrapper/libcef_dll_wrapper.a): \
    error("libcef_dll_wrapper not built — run cmake per spikes/cef/README.md")

INCLUDEPATH += $$CEF_ROOT $$CEF_ROOT/include
# CEF headers force their own defines; match what the distro CMake used.
DEFINES += NDEBUG

LIBS += $$CEF_ROOT/build/libcef_dll_wrapper/libcef_dll_wrapper.a
LIBS += -L$$CEF_ROOT/Release -lcef
# libcef.so needs the usual Chromium client libs.
LIBS += -lX11 -lpthread -ldl -latomic
QMAKE_RPATHDIR += $$CEF_ROOT/Release
