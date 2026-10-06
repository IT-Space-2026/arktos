INCLUDEPATH += $$PWD $$PWD/../../tools/htmlToXBel
DEPENDPATH += $$PWD

# Netscape-HTML import uses the same bounded parser as the
# htmlToXBel tool instead of spawning it as a subprocess (FRZ01).
SOURCES += $$PWD/../../tools/htmlToXBel/converter.cpp

HEADERS += \
    $$PWD/addbookmarkdialog.h \
    $$PWD/bookmarksdialog.h \
    $$PWD/bookmarksmanager.h \
    $$PWD/bookmarksmenu.h \
    $$PWD/bookmarksmodel.h \
    $$PWD/bookmarkstoolbar.h \
    $$PWD/bookmarknode.h

SOURCES += \
    $$PWD/addbookmarkdialog.cpp \
    $$PWD/bookmarksdialog.cpp \
    $$PWD/bookmarksmanager.cpp \
    $$PWD/bookmarksmenu.cpp \
    $$PWD/bookmarksmodel.cpp \
    $$PWD/bookmarkstoolbar.cpp \
    $$PWD/bookmarknode.cpp

FORMS += \
    $$PWD/addbookmarkdialog.ui \
    $$PWD/bookmarksdialog.ui

include(xbel/xbel.pri)
