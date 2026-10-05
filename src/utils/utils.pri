INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

# MIG04: webpageproxy.* and networkaccessmanagerproxy.* were deleted —
# the QWebPage-tagging indirection has no WebEngine equivalent.
# MIG03: edittableview.* and lineedit.*/lineedit_p.h moved to the live
# lists in src.pri (needed by the cookiejar dialogs).
HEADERS += \
    editlistview.h \
    edittreeview.h \
    languagemanager.h \
    singleapplication.h \
    squeezelabel.h \
    treesortfilterproxymodel.h

SOURCES += \
    editlistview.cpp \
    edittreeview.cpp \
    languagemanager.cpp \
    singleapplication.cpp \
    squeezelabel.cpp \
    treesortfilterproxymodel.cpp

win32 {
    HEADERS += explorerstyle.h
    SOURCES += explorerstyle.cpp
    LIBS += -lgdi32
}

