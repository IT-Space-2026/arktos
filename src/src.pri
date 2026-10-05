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
# Only main.cpp (listed in src.pro) is compiled for now. The original
# file lists are kept below as the porting checklist — each MIG task
# uncomments the files it ports. Ownership map: .devin/Arora-Task.md.
#
# FORMS += \
#     aboutdialog.ui \              # TODO(MIG14)
#     autofilldialog.ui \           # TODO(MIG10)
#     acceptlanguagedialog.ui \     # TODO(MIG11)
#     (downloaditem.ui/downloads.ui live — MIG05)
#     searchbanner.ui \             # TODO(MIG08)
#     settings.ui                   # TODO(MIG11)
#
# HEADERS += \
#     aboutdialog.h \               # TODO(MIG14)
#     acceptlanguagedialog.h \      # TODO(MIG11)
#     autofilldialog.h \            # TODO(MIG10)
#     autofillmanager.h \           # TODO(MIG10)
#     browserapplication.h \        # TODO(MIG15)
#     browsermainwindow.h \         # TODO(MIG14)
#     clearprivatedata.h \          # TODO(MIG11)
#     (downloadmanager.h live — MIG05)
#     (modelmenu.h live — MIG06 dep)
#     modeltoolbar.h \              # TODO(MIG07)
#     plaintexteditsearch.h \       # TODO(MIG12)
#     searchbar.h \                 # TODO(MIG08)
#     settings.h \                  # TODO(MIG11)
#     sourcehighlighter.h \         # TODO(MIG12)
#     sourceviewer.h \              # TODO(MIG12)
#     tabbar.h \                    # TODO(MIG14)
#     tabwidget.h \                 # TODO(MIG14)
#     toolbarsearch.h \             # TODO(MIG08)
#     (webactionmapper/webpage/webview moved to live list — MIG02 done)
#     (autosaver/clearbutton/searchbutton/searchlineedit live — MIG03)
#     (webpageproxy + networkaccessmanagerproxy deleted — MIG04)
#     webviewsearch.h               # TODO(MIG12)
#
# SOURCES += \
#     aboutdialog.cpp \             # TODO(MIG14)
#     acceptlanguagedialog.cpp \    # TODO(MIG11)
#     autofilldialog.cpp \          # TODO(MIG10)
#     autofillmanager.cpp \         # TODO(MIG10)
#     browserapplication.cpp \      # TODO(MIG15)
#     browsermainwindow.cpp \       # TODO(MIG14)
#     clearprivatedata.cpp \        # TODO(MIG11)
#     (downloadmanager.cpp live — MIG05)
#     (modelmenu.cpp live — MIG06 dep)
#     modeltoolbar.cpp \            # TODO(MIG07)
#     plaintexteditsearch.cpp \     # TODO(MIG12)
#     searchbar.cpp \               # TODO(MIG08)
#     settings.cpp \                # TODO(MIG11)
#     sourcehighlighter.cpp \       # TODO(MIG12)
#     sourceviewer.cpp \            # TODO(MIG12)
#     tabbar.cpp \                  # TODO(MIG14)
#     tabwidget.cpp \               # TODO(MIG14)
#     toolbarsearch.cpp \           # TODO(MIG08)
#     webviewsearch.cpp             # TODO(MIG12)
#
# include(adblock/adblock.pri)        # TODO(MIG09)
# include(bookmarks/bookmarks.pri)    # TODO(MIG07)
include(history/history.pri)          # MIG06 done
# include(locationbar/locationbar.pri)  # TODO(MIG08)
include(network/network.pri)          # MIG04 done; cookiejar.pri split out (MIG03)
# include(opensearch/opensearch.pri)  # TODO(MIG08)
# include(qwebplugins/qwebplugins.pri)  # TODO(MIG13): remove, Flash is dead
# include(utils/utils.pri)            # TODO(MIG02..MIG13 per file)
# include(useragent/useragent.pri)    # TODO(MIG14)
# ------------------------------------------------------------------------

include(network/cookiejar/cookiejar.pri)

FORMS += \
    downloaditem.ui \
    downloads.ui
HEADERS += \
    autosaver.h \
    browserpaths.h \
    clearbutton.h \
    downloadmanager.h \
    modelmenu.h \
    searchbutton.h \
    searchlineedit.h \
    webactionmapper.h \
    webpage.h \
    webview.h \
    utils/edittableview.h \
    utils/edittreeview.h \
    utils/lineedit.h \
    utils/lineedit_p.h \
    utils/squeezelabel.h \
    utils/treesortfilterproxymodel.h
SOURCES += \
    autosaver.cpp \
    clearbutton.cpp \
    downloadmanager.cpp \
    modelmenu.cpp \
    searchbutton.cpp \
    searchlineedit.cpp \
    webactionmapper.cpp \
    webpage.cpp \
    webview.cpp \
    utils/edittableview.cpp \
    utils/edittreeview.cpp \
    utils/lineedit.cpp \
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
