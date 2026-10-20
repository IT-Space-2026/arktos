TEMPLATE = app
TARGET = htmlToXBel
DEPENDPATH += .
INCLUDEPATH += .

win32: CONFIG += console
mac:CONFIG -= app_bundle

# QtGui for QTextDocumentFragment (HTML entity decoding); no engine needed.
QT += gui

# Input
HEADERS += converter.h
SOURCES += converter.cpp main.cpp

RCC_DIR     = $$PWD/.rcc
UI_DIR      = $$PWD/.ui
MOC_DIR     = $$PWD/.moc
OBJECTS_DIR = $$PWD/.obj

# `make check` here converts every bundled browser-export fixture
# (tests/*-bookmarks.html) and requires real <bookmark> output — keeps
# the fixtures honest as the converter changes.  Reached from the root
# `make check` via the check-htmltoxbel target.
unix {
    check.target = check
    check.commands = $$OUT_PWD/htmlToXBel $$PWD/tests/firefox3-bookmarks.html > /dev/null && \
        $$OUT_PWD/htmlToXBel $$PWD/tests/ie7-bookmarks.html > /dev/null && \
        $$OUT_PWD/htmlToXBel $$PWD/tests/opera9-bookmarks.html > /dev/null && \
        echo "htmlToXBel fixtures OK"
    QMAKE_EXTRA_TARGETS += check
}

include(../../BuildProcess/install.pri)

!mac {
unix {
    INSTALLS += man man-compress

    man.path = $$DATADIR/man/man1
    man.files += data/htmlToXBel.1

    man-compress.path = $$DATADIR/man/man1
    man-compress.extra = "" "gzip -9 -f \$(INSTALL_ROOT)/$$DATADIR/man/man1/htmlToXBel.1" ""
    man-compress.depends = install_man
}
}
