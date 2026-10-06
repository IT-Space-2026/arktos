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

# `make check-fuzz` runs the FUZZ01 libFuzzer harnesses — they build in
# their own clang dir, so no dependency on (or pollution of) the
# in-tree gcc build.  Pass a budget: FUZZ_SECONDS=60.
check-fuzz.target = check-fuzz
check-fuzz.commands = ./.devin/run-fuzz.sh
QMAKE_EXTRA_TARGETS += check-fuzz

# `make check-sanitize` runs the HARD01 ASan+UBSan sweep — builds its
# own instrumented copy of the tree in /tmp (sanitize.pri), runs the
# autotests + smokes, and gates on zero sanitizer reports.
check-sanitize.target = check-sanitize
check-sanitize.commands = ./.devin/check-sanitize.sh
QMAKE_EXTRA_TARGETS += check-sanitize

# `make check-static` runs the STAT01 analyzer sweep — builds its own
# copy of the tree with gcc -fanalyzer (analyzer.pri), then replays
# every compile line through clang++ --analyze, and writes the deduped
# findings to .devin/STAT01-report.md (report-only, not a gate).
check-static.target = check-static
check-static.commands = ./.devin/check-static.sh
QMAKE_EXTRA_TARGETS += check-static
