INCLUDEPATH += $$PWD
DEPENDPATH += $$PWD

# MIG02: webpageproxy.* and networkaccessmanagerproxy.* were moved to the
# live lists in src.pri (ported to Qt WebEngine).
HEADERS += \
    editlistview.h \
    edittableview.h \
    edittreeview.h \
    languagemanager.h \
    lineedit.h \
    lineedit_p.h \
    singleapplication.h \
    squeezelabel.h \
    treesortfilterproxymodel.h

SOURCES += \
    editlistview.cpp \
    edittableview.cpp \
    edittreeview.cpp \
    languagemanager.cpp \
    lineedit.cpp \
    singleapplication.cpp \
    squeezelabel.cpp \
    treesortfilterproxymodel.cpp

win32 {
    HEADERS += explorerstyle.h
    SOURCES += explorerstyle.cpp
    LIBS += -lgdi32
}

