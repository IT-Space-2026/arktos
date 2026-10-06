# STAT01: GCC -fanalyzer build variant.
# Build an instrumented copy of the tree with the default (linux-g++)
# spec:
#     qmake CONFIG+=analyzer && make
# The clang static analyzer cannot share this pass — `--analyze`
# produces no objects — so .devin/check-static.sh runs it separately
# per translation unit (clang++ --analyze on the extracted compile
# lines).  .devin/check-static.sh automates both passes + the report.
analyzer {
    QMAKE_CXXFLAGS += -fanalyzer
}
