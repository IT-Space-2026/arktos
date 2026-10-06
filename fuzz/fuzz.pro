# libFuzzer harnesses for the app's hostile-input parsers (FUZZ01).
# Not part of the top-level SUBDIRS build — they need clang +
# -fsanitize=fuzzer.  Build/run via .devin/run-fuzz.sh or `make
# check-fuzz` from the top level.

TEMPLATE = subdirs
SUBDIRS = \
    adblockrule \
    cookiejarstate \
    historyformat \
    htmltoxbel \
    opensearchreader \
    securestore \
    streamingutils \
    xbel
