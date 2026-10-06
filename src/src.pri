CONFIG += qt warn_on

win32:Debug:CONFIG += console

INCLUDEPATH += $$PWD $$PWD/utils
DEPENDPATH += $$PWD $$PWD/utils

QT += core gui widgets network printsupport webenginewidgets webchannel uitools core5compat

# Share object files for faster compiling
RCC_DIR     = $$PWD/.rcc
UI_DIR      = $$PWD/.ui
MOC_DIR     = $$PWD/.moc
OBJECTS_DIR = $$PWD/.obj

exists(../.git/HEAD) {
    GITVERSION=$$system(git log -n1 --pretty=format:%h)
    !isEmpty(GITVERSION) {
        GITCHANGENUMBER=$$system(git log --pretty=format:%h | wc -l)
        DEFINES += GITVERSION=\"\\\"$$GITVERSION\\\"\"
        DEFINES += GITCHANGENUMBER=\"\\\"$$GITCHANGENUMBER\\\"\"
    }
}

# --- Qt6 migration skeleton (MIG01) ------------------------------------
# Only main.cpp (listed in src.pro) is compiled plus the live lists
# below. The original file lists are kept as the porting checklist —
# each MIG task moves its files into the live lists.
# Ownership map: .devin/Arora-Task.md.
#
# MIG14 moved in: aboutdialog, browsermainwindow, tabbar, tabwidget,
# useragent.pri — plus browserapplication, which the chrome needs for
# linking (full app-entry wiring is still MIG15).
#
include(adblock/adblock.pri)          # MIG09 done
include(bookmarks/bookmarks.pri)      # MIG07 done
include(history/history.pri)          # MIG06 done
include(locationbar/locationbar.pri)  # MIG08 done
include(network/network.pri)          # MIG04 done; cookiejar.pri split out (MIG03)
include(opensearch/opensearch.pri)    # MIG08 done
include(useragent/useragent.pri)      # MIG14 done
# MIG13: qwebplugins/ deleted (plugin machinery removed per user
# directive); utils.pri retired — remaining utils/ files are in the
# live lists below.
# ------------------------------------------------------------------------

include(network/cookiejar/cookiejar.pri)

FORMS += \
    aboutdialog.ui \
    acceptlanguagedialog.ui \
    autofilldialog.ui \
    downloaditem.ui \
    downloads.ui \
    searchbanner.ui \
    settings.ui
HEADERS += \
    aboutdialog.h \
    acceptlanguagedialog.h \
    autofilldialog.h \
    autofillmanager.h \
    autosaver.h \
    browserapplication.h \
    browsermainwindow.h \
    browserpaths.h \
    browserprofile.h \
    clearbutton.h \
    clearprivatedata.h \
    downloadmanager.h \
    modelmenu.h \
    modeltoolbar.h \
    plaintexteditsearch.h \
    searchbar.h \
    searchbutton.h \
    searchlineedit.h \
    settings.h \
    sourcehighlighter.h \
    sourceviewer.h \
    tabbar.h \
    tabwidget.h \
    toolbarsearch.h \
    webactionmapper.h \
    webpage.h \
    webview.h \
    webviewsearch.h \
    utils/edittableview.h \
    utils/edittreeview.h \
    utils/languagemanager.h \
    utils/lineedit.h \
    utils/lineedit_p.h \
    utils/singleapplication.h \
    utils/squeezelabel.h \
    utils/treesortfilterproxymodel.h
SOURCES += \
    aboutdialog.cpp \
    acceptlanguagedialog.cpp \
    autofilldialog.cpp \
    autofillmanager.cpp \
    autosaver.cpp \
    browserapplication.cpp \
    browsermainwindow.cpp \
    browserprofile.cpp \
    clearbutton.cpp \
    clearprivatedata.cpp \
    downloadmanager.cpp \
    modelmenu.cpp \
    modeltoolbar.cpp \
    plaintexteditsearch.cpp \
    searchbar.cpp \
    searchbutton.cpp \
    searchlineedit.cpp \
    settings.cpp \
    sourcehighlighter.cpp \
    sourceviewer.cpp \
    tabbar.cpp \
    tabwidget.cpp \
    toolbarsearch.cpp \
    webactionmapper.cpp \
    webpage.cpp \
    webview.cpp \
    webviewsearch.cpp \
    utils/edittableview.cpp \
    utils/edittreeview.cpp \
    utils/languagemanager.cpp \
    utils/lineedit.cpp \
    utils/singleapplication.cpp \
    utils/squeezelabel.cpp \
    utils/treesortfilterproxymodel.cpp

RESOURCES += \
    $$PWD/data/data.qrc \
    $$PWD/data/graphics/graphics.qrc \
    $$PWD/data/searchengines/searchengines.qrc \
    $$PWD/htmls/htmls.qrc

DISTFILES += ../AUTHORS \
    ../ChangeLog \
    ../LICENSE.GPL2 \
    ../LICENSE.GPL3 \
    ../README

win32 {
    RC_FILE = $$PWD/browser.rc
    LIBS += -luser32 -ladvapi32
}

mac {
    ICON = browser.icns
    QMAKE_INFO_PLIST = Info_mac.plist
}

unix {
    PKGDATADIR = $$DATADIR/arora
    DEFINES += DATADIR=\\\"$$DATADIR\\\" PKGDATADIR=\\\"$$PKGDATADIR\\\"
}
