TEMPLATE = app

TARGET = arktos
mac {
    TARGET = Arktos
    ICON = $$PWD/../BuildProcess/browser.icns
    QMAKE_INFO_PLIST = $$PWD/../BuildProcess/Info_mac.plist

    # EXT07/MAC06: the vendored default extensions ride inside the
    # .app — the !mac INSTALLS path never runs on macOS, so without
    # bundle data the dmg ships no share/arktos/extensions and
    # bundledExtensionsPath() can never resolve.  QMAKE_BUNDLE_DATA
    # copies at build time, so macdeployqt -dmg carries it.
    bundledext.path = Contents/Resources/share/arktos/extensions
    bundledext.files = $$PWD/data/extensions/bitwarden-*.zip \
                       $$PWD/data/extensions/bitwarden-LICENSE*.txt \
                       $$PWD/data/extensions/PROVENANCE.txt
    QMAKE_BUNDLE_DATA += bundledext
}

DEFINES += \
    QT_NO_CAST_FROM_ASCII \
    QT_NO_CAST_TO_ASCII \
    QT_STRICT_ITERATORS \

include(../BuildProcess/install.pri)

include(src.pri)

SOURCES += main.cpp

DESTDIR = ../

include(locale/locale.pri)

!mac {
unix {
    INSTALLS += translations desktop iconxpm iconsvg icon16 icon32 icon128 icon512 man man-compress useragentdata bundledext sandboxlauncher

    translations.path = $$PKGDATADIR
    translations.files += .qm/locale

    desktop.path = $$DATADIR/applications
    desktop.files += ../BuildProcess/arktos.desktop

    iconxpm.path = $$DATADIR/pixmaps
    iconxpm.files += data/arktos.xpm

    iconsvg.path = $$DATADIR/icons/hicolor/scalable/apps
    iconsvg.files += data/arktos.svg

    icon16.path = $$DATADIR/icons/hicolor/16x16/apps
    icon16.files += data/16x16/arktos.png

    icon32.path = $$DATADIR/icons/hicolor/32x32/apps
    icon32.files += data/32x32/arktos.png

    icon128.path = $$DATADIR/icons/hicolor/128x128/apps
    icon128.files += data/128x128/arktos.png

    icon512.path = $$DATADIR/icons/hicolor/512x512/apps
    icon512.files += data/512x512/arktos.png

    # Distros/users can drop a customized useragents.xml into PKGDATADIR;
    # UserAgentMenu prefers it over the compiled-in resource.
    useragentdata.path = $$PKGDATADIR
    useragentdata.files += useragent/useragents.xml

    # EXT07: bundled default extensions (Bitwarden — see
    # data/extensions/PROVENANCE.txt for version/pinning) install
    # onto each extension-fresh profile on first run.
    bundledext.path = $$PKGDATADIR/extensions
    bundledext.files += data/extensions/bitwarden-*.zip \
                        data/extensions/bitwarden-LICENSE*.txt \
                        data/extensions/PROVENANCE.txt

    man.path = $$DATADIR/man/man1
    man.files += data/arktos.1

    # SAND01: the generated bwrap launcher installs next to the binary
    # as `arktos-sandbox`.  `executable` makes qmake install it 755.
    sandboxlauncher.path = $$BINDIR
    sandboxlauncher.files += sandbox/arktos-sandbox
    sandboxlauncher.CONFIG += executable

    man-compress.path = $$DATADIR/man/man1
    man-compress.extra = "" "gzip -9 -f \$(INSTALL_ROOT)/$$DATADIR/man/man1/arktos.1" ""
    man-compress.depends = install_man

    GNOME_DEFAULT_APPS_PATH = $$system(pkg-config --variable=defappsdir gnome-default-applications)

    !isEmpty(GNOME_DEFAULT_APPS_PATH) {
        INSTALLS += gnome-default-app

        gnome-default-app.path = $$GNOME_DEFAULT_APPS_PATH
        gnome-default-app.files = ../BuildProcess/arktos.xml
    }
}
}
