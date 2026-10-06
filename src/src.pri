CONFIG += qt warn_on

win32:Debug:CONFIG += console

include($$PWD/../coverage.pri)

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
# linking (MIG15 wired it up as the real application object).
#
include(adblock/adblock.pri)          # MIG09 done
include(bookmarks/bookmarks.pri)      # MIG07 done
include(extensions/extensions.pri)    # EXT01 done
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
    $$PWD/aboutdialog.ui \
    $$PWD/acceptlanguagedialog.ui \
    $$PWD/autofilldialog.ui \
    $$PWD/downloaditem.ui \
    $$PWD/downloads.ui \
    $$PWD/searchbanner.ui \
    $$PWD/settings.ui
HEADERS += \
    $$PWD/aboutdialog.h \
    $$PWD/acceptlanguagedialog.h \
    $$PWD/autofilldialog.h \
    $$PWD/autofillmanager.h \
    $$PWD/autosaver.h \
    $$PWD/browserapplication.h \
    $$PWD/browsermainwindow.h \
    $$PWD/browserpaths.h \
    $$PWD/browserprofile.h \
    $$PWD/clearbutton.h \
    $$PWD/clearprivatedata.h \
    $$PWD/downloadmanager.h \
    $$PWD/modelmenu.h \
    $$PWD/modeltoolbar.h \
    $$PWD/plaintexteditsearch.h \
    $$PWD/searchbar.h \
    $$PWD/searchbutton.h \
    $$PWD/searchlineedit.h \
    $$PWD/securestore.h \
    $$PWD/settings.h \
    $$PWD/sourcehighlighter.h \
    $$PWD/sourceviewer.h \
    $$PWD/tabbar.h \
    $$PWD/tabwidget.h \
    $$PWD/toolbarsearch.h \
    $$PWD/webactionmapper.h \
    $$PWD/webpage.h \
    $$PWD/webview.h \
    $$PWD/webviewsearch.h \
    $$PWD/utils/edittableview.h \
    $$PWD/utils/edittreeview.h \
    $$PWD/utils/languagemanager.h \
    $$PWD/utils/lineedit.h \
    $$PWD/utils/lineedit_p.h \
    $$PWD/utils/singleapplication.h \
    $$PWD/utils/squeezelabel.h \
    $$PWD/utils/treesortfilterproxymodel.h
SOURCES += \
    $$PWD/aboutdialog.cpp \
    $$PWD/acceptlanguagedialog.cpp \
    $$PWD/autofilldialog.cpp \
    $$PWD/autofillmanager.cpp \
    $$PWD/autosaver.cpp \
    $$PWD/browserapplication.cpp \
    $$PWD/browsermainwindow.cpp \
    $$PWD/browserprofile.cpp \
    $$PWD/clearbutton.cpp \
    $$PWD/clearprivatedata.cpp \
    $$PWD/downloadmanager.cpp \
    $$PWD/modelmenu.cpp \
    $$PWD/modeltoolbar.cpp \
    $$PWD/plaintexteditsearch.cpp \
    $$PWD/searchbar.cpp \
    $$PWD/searchbutton.cpp \
    $$PWD/searchlineedit.cpp \
    $$PWD/securestore.cpp \
    $$PWD/settings.cpp \
    $$PWD/sourcehighlighter.cpp \
    $$PWD/sourceviewer.cpp \
    $$PWD/tabbar.cpp \
    $$PWD/tabwidget.cpp \
    $$PWD/toolbarsearch.cpp \
    $$PWD/webactionmapper.cpp \
    $$PWD/webpage.cpp \
    $$PWD/webview.cpp \
    $$PWD/webviewsearch.cpp \
    $$PWD/utils/edittableview.cpp \
    $$PWD/utils/edittreeview.cpp \
    $$PWD/utils/languagemanager.cpp \
    $$PWD/utils/lineedit.cpp \
    $$PWD/utils/singleapplication.cpp \
    $$PWD/utils/squeezelabel.cpp \
    $$PWD/utils/treesortfilterproxymodel.cpp

RESOURCES += \
    $$PWD/data/data.qrc \
    $$PWD/data/graphics/graphics.qrc \
    $$PWD/data/searchengines/searchengines.qrc \
    $$PWD/htmls/htmls.qrc

DISTFILES += $$PWD/../AUTHORS \
    $$PWD/../ChangeLog \
    $$PWD/../LICENSE.GPL2 \
    $$PWD/../LICENSE.GPL3 \
    $$PWD/../README

win32 {
    RC_FILE = $$PWD/browser.rc
    LIBS += -luser32 -ladvapi32
}

mac {
    ICON = $$PWD/browser.icns
    QMAKE_INFO_PLIST = $$PWD/Info_mac.plist
}

unix {
    PKGDATADIR = $$DATADIR/arora
    DEFINES += DATADIR=\\\"$$DATADIR\\\" PKGDATADIR=\\\"$$PKGDATADIR\\\"
}
