lessThan(QT_MAJOR_VERSION, 6) {
    error("Arora requires Qt 6 or greater")
}

TEMPLATE = subdirs
SUBDIRS  = src tools autotests
CONFIG += ordered

unix {
    # .PHONY: doc — `make doc`/`make dox` builds the doxygen API
    # reference into doc/ (two separate targets; a space-separated
    # `doc dox` value emits one target named "doc dox").
    doxygen.target = doc
    doxygen.commands = doxygen Doxyfile
    doxygen.depends = Doxyfile
    dox.target = dox
    dox.commands = doxygen Doxyfile
    dox.depends = Doxyfile
    QMAKE_EXTRA_TARGETS += doxygen dox
}

# `make check` builds and runs the QtTest suite headless.  The
# check-warnings gate runs first so a warning regression fails the
# suite before a single test executes.
check.target = check
check.commands = cd autotests && ./runTests.sh
check.depends = check-warnings check-supplychain sub-src sub-autotests
QMAKE_EXTRA_TARGETS += check

# `make check-supplychain` runs the SEC21 Rust supply-chain vetting
# gate — `cargo deny` (advisories incl. yanked, license allow-list,
# duplicate/wildcard bans, crates.io-only sources) + `cargo audit`
# over every crate's committed Cargo.lock.  A vulnerability in a dep
# we ship fails the build.  Machines without cargo-deny/cargo-audit
# skip cleanly, same as a no-rust build.
check-supplychain.target = check-supplychain
check-supplychain.commands = ./.devin/check-supplychain.sh
QMAKE_EXTRA_TARGETS += check-supplychain

# `make bundle` produces the self-contained relocatable Linux bundle in
# dist/ (PACK01). `make check-bundle` rebuilds it in a scratch dir and
# verifies self-containment + offscreen and native-Wayland startup inside
# a bwrap sandbox with the dev Qt install hidden.
bundle.target = bundle
bundle.commands = ./BuildProcess/bundle-linux.sh
QMAKE_EXTRA_TARGETS += bundle

check-bundle.target = check-bundle
check-bundle.commands = ./.devin/check-bundle.sh
QMAKE_EXTRA_TARGETS += check-bundle

# `make check-warnings` is the WRN01 zero-warning gate — it builds a
# throwaway copy of the tree in /tmp (never polluting the in-tree
# build) and fails if compiling the shipping code (src/ + tools/)
# emits any warning or error.  No -Werror in the shipped config; this
# target is the gate.
check-warnings.target = check-warnings
check-warnings.commands = ./.devin/check-warnings.sh
QMAKE_EXTRA_TARGETS += check-warnings

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

# `make check-leaks` runs the MEM01 memory-leak sweep — LeakSanitizer
# over an instrumented copy of the tree, then valgrind memcheck over
# the in-tree build.  Gates on zero leaks allocated in Arora code;
# writes .devin/LEAKS.md.  Needs an in-tree build for the valgrind
# phase (or ARORA_LEAKS_VALGRIND=0 to skip it).
check-leaks.target = check-leaks
check-leaks.commands = ./.devin/check-leaks.sh
QMAKE_EXTRA_TARGETS += check-leaks
