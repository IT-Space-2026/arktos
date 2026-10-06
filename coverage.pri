# COV01: clang source-based coverage instrumentation.
# Build an instrumented tree with:
#     qmake -spec linux-clang CONFIG+=coverage
# then run the binaries under LLVM_PROFILE_FILE and merge/report with
# llvm-profdata-18 / llvm-cov-18 (.devin/check-coverage.sh automates it).
coverage {
    # -runtime-counter-relocation is required for LLVM_PROFILE_FILE's
    # %c continuous mode on ELF/PIE (otherwise the runtime aborts the
    # write with "__llvm_profile_counter_bias is undefined").
    QMAKE_CFLAGS   += -fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation
    QMAKE_CXXFLAGS += -fprofile-instr-generate -fcoverage-mapping -mllvm -runtime-counter-relocation
    QMAKE_LFLAGS   += -fprofile-instr-generate -fcoverage-mapping
}
