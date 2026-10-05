lessThan(QT_MAJOR_VERSION, 6) {
    error("Arora requires Qt 6 or greater")
}

TEMPLATE = subdirs
SUBDIRS  = src
# TODO(TST01): re-enable autotests once the suite is ported to Qt6.
# TODO(AUD01): tools/ utilities are Qt4-era; audit before re-enabling.
CONFIG += ordered

unix {
    # this is an ugly work around to do .PHONY: doc
    doxygen.target = doc dox
    doxygen.commands = doxygen Doxyfile
    doxygen.depends = Doxyfile
    QMAKE_EXTRA_TARGETS += doxygen
}
