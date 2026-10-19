# WK02 spike harness — NOT in the top-level SUBDIRS build (make check untouched).
# Requires: source .devin/webkit-env.sh && source .devin/qt-env.sh first
#   cd spikes/webkit/wpe-harness && ./build-harness.sh
TEMPLATE = app
TARGET = wpe-harness
QT += widgets
# no_keywords: glib headers use a struct member named "signals" — Qt's
# signals/slots macros must stay out of the way (we only use Q_* forms).
CONFIG += c++17 warn_on link_pkgconfig no_keywords

PKGCONFIG += wpe-webkit-2.0 wpe-platform-headless-2.0

SOURCES += wpe-harness.cpp

# The WK00 sid-glibc recipe (see ~/wpe/ACQUIRED.md + .devin/webkit-env.sh):
# sid ld.so as interp, DT_RPATH (transitive) over wpe+glibc libdirs, the
# g_sort_array compat shim retained via --no-as-needed, and sid libc/libm
# explicitly for the GLIBC_2.4x versioned refs libWPEWebKit carries.
ARKTOS_WPE_LIBDIR = $$(ARKTOS_WPE_LIBDIR)
ARKTOS_WPE_GLIBC = $$(ARKTOS_WPE_GLIBC)
ARKTOS_WPE_DYNAMIC_LINKER = $$(ARKTOS_WPE_DYNAMIC_LINKER)
ARKTOS_WPE_COMPAT = $$(ARKTOS_WPE_COMPAT)
isEmpty(ARKTOS_WPE_DYNAMIC_LINKER): \
    error("ARKTOS_WPE_* env missing — source .devin/webkit-env.sh before qmake")

LIBS += -L$$ARKTOS_WPE_LIBDIR
LIBS += -Wl,--no-as-needed $$ARKTOS_WPE_COMPAT -Wl,--as-needed
LIBS += $$ARKTOS_WPE_GLIBC/libc.so.6 $$ARKTOS_WPE_GLIBC/libm.so.6
QMAKE_LFLAGS += -Wl,--dynamic-linker,$$ARKTOS_WPE_DYNAMIC_LINKER -Wl,--disable-new-dtags
QMAKE_RPATHDIR += $$ARKTOS_WPE_LIBDIR $$ARKTOS_WPE_GLIBC
