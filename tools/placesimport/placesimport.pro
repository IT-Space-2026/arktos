TEMPLATE = app
TARGET = arora-placesimport
DEPENDPATH += .
INCLUDEPATH += .

win32|os2: CONFIG += console
mac:CONFIG -= app_bundle

QT += sql widgets

# Match src.pro so the shared src/.obj objects are flag-identical.
DEFINES += \
    QT_NO_CAST_FROM_ASCII \
    QT_NO_CAST_TO_ASCII \
    QT_STRICT_ITERATORS \

# Input
SOURCES += main_placesimport.cpp

include(../../BuildProcess/install.pri)
# Reuses the whole ported tree (shared src/.obj objects) for the history
# store, single-instance guard and serialization code — same approach the
# Qt4 tool took.
include(../../src/src.pri)

!mac {
unix {
    INSTALLS += man man-compress

    man.path = $$DATADIR/man/man1
    man.files += data/arora-placesimport.1

    man-compress.path = $$DATADIR/man/man1
    man-compress.extra = "" "gzip -9 -f \$(INSTALL_ROOT)/$$DATADIR/man/man1/arora-placesimport.1" ""
    man-compress.depends = install_man
}
}
