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
#     (autofilldialog.ui live — MIG10)
#     (acceptlanguagedialog.ui live — MIG11)
#     (downloaditem.ui/downloads.ui live — MIG05)
#     (searchbanner.ui live — MIG08)
#     (settings.ui live — MIG11)
#
# HEADERS += \
#     aboutdialog.h \               # TODO(MIG14)
#     (acceptlanguagedialog.h live — MIG11)
#     (autofilldialog.h/autofillmanager.h live — MIG10)
#     browserapplication.h \        # TODO(MIG15)
#     browsermainwindow.h \         # TODO(MIG14)
#     (clearprivatedata.h live — MIG11)
#     (downloadmanager.h live — MIG05)
#     (modelmenu.h live — MIG06 dep)
#     (modeltoolbar.h live — MIG07)
#     (plaintexteditsearch.h live — MIG12)
#     (searchbar.h live — MIG08)
#     (settings.h/browserprofile.h live — MIG11)
#     (sourcehighlighter.h live — MIG12)
#     (sourceviewer.h live — MIG12)
#     tabbar.h \                    # TODO(MIG14)
#     tabwidget.h \                 # TODO(MIG14)
#     (toolbarsearch.h live — MIG08)
#     (webactionmapper/webpage/webview moved to live list — MIG02 done)
#     (autosaver/clearbutton/searchbutton/searchlineedit live — MIG03)
#     (languagemanager live — MIG11 dep)
#     (webpageproxy + networkaccessmanagerproxy deleted — MIG04)
#     (webviewsearch.h live — MIG12)
#
# SOURCES += \
#     aboutdialog.cpp \             # TODO(MIG14)
#     (acceptlanguagedialog.cpp live — MIG11)
#     (autofilldialog.cpp/autofillmanager.cpp live — MIG10)
#     browserapplication.cpp \      # TODO(MIG15)
#     browsermainwindow.cpp \       # TODO(MIG14)
#     (clearprivatedata.cpp live — MIG11)
#     (downloadmanager.cpp live — MIG05)
#     (modelmenu.cpp live — MIG06 dep)
#     (modeltoolbar.cpp live — MIG07)
#     (plaintexteditsearch.cpp live — MIG12)
#     (searchbar.cpp live — MIG08)
#     (settings.cpp/browserprofile.cpp live — MIG11)
#     (sourcehighlighter.cpp live — MIG12)
#     (sourceviewer.cpp live — MIG12)
#     tabbar.cpp \                  # TODO(MIG14)
#     tabwidget.cpp \               # TODO(MIG14)
#     (toolbarsearch.cpp live — MIG08)
#     (languagemanager live — MIG11 dep)
#     (webviewsearch.cpp live — MIG12)
#
include(adblock/adblock.pri)          # MIG09 done
include(bookmarks/bookmarks.pri)      # MIG07 done
include(history/history.pri)          # MIG06 done
include(locationbar/locationbar.pri)  # MIG08 done
include(network/network.pri)          # MIG04 done; cookiejar.pri split out (MIG03)
include(opensearch/opensearch.pri)    # MIG08 done
# include(useragent/useragent.pri)    # TODO(MIG14)
# MIG13: qwebplugins/ deleted (plugin machinery removed per user
# directive); utils.pri retired — remaining utils/ files are in the
# live lists below.
# ------------------------------------------------------------------------

include(network/cookiejar/cookiejar.pri)

FORMS += \
    acceptlanguagedialog.ui \
    autofilldialog.ui \
    downloaditem.ui \
    downloads.ui \
    searchbanner.ui \
    settings.ui
HEADERS += \
    acceptlanguagedialog.h \
    autofilldialog.h \
    autofillmanager.h \
    autosaver.h \
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
    acceptlanguagedialog.cpp \
    autofilldialog.cpp \
    autofillmanager.cpp \
    autosaver.cpp \
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
