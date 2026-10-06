lessThan(QT_MAJOR_VERSION, 6) {
    error("Arora requires Qt 6 or greater")
}

TEMPLATE = subdirs
SUBDIRS  = src tools autotests
CONFIG += ordered

unix {
    # this is an ugly work around to do .PHONY: doc
    doxygen.target = doc dox
    doxygen.commands = doxygen Doxyfile
    doxygen.depends = Doxyfile
    QMAKE_EXTRA_TARGETS += doxygen
}

# `make check` builds and runs the QtTest suite headless.
check.target = check
check.commands = cd autotests && ./runTests.sh
check.depends = sub-src sub-autotests
QMAKE_EXTRA_TARGETS += check

# `make check-coverage` runs the COV01 clang source-based coverage
# harness — it builds its own instrumented copy of the tree, so it does
# not depend on (or pollute) the in-tree build.
check-coverage.target = check-coverage
check-coverage.commands = ./.devin/check-coverage.sh
QMAKE_EXTRA_TARGETS += check-coverage
