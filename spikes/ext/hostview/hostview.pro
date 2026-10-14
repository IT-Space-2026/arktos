# EXT13 Arch-A spike harness — hidden-QWebEngineView extension host.
# NOT in the top-level SUBDIRS build (same convention as
# spikes/servo/harness).  Build+run:
#   source .devin/qt-env.sh
#   cd spikes/ext/hostview && qmake && make
#   QT_QPA_PLATFORM=offscreen QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu \
#       ./hostview [fixture-dir] [--no-bitwarden]
TEMPLATE = app
TARGET = hostview
QT += widgets webenginewidgets webenginecore network
CONFIG += c++17 warn_on

SOURCES += main.cpp
