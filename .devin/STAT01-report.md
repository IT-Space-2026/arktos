# Static-analysis sweep (STAT01)

Generated: 2026-10-06 21:06 UTC on commit b16d5a6
Toolchains: g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0 / Ubuntu clang version 18.1.3 (1ubuntu1)

clang-tidy, cppcheck and scan-build are NOT installed on this
box and cannot be (no sudo) — the sweep is limited to
`g++ -fanalyzer` and the clang static analyzer
(`clang++ --analyze`).

## Reproduce

    source .devin/qt-env.sh && make check-static

A copy of the tree is built in a throwaway dir with
`CONFIG+=analyzer` (-fanalyzer on every TU); then each compile
line is replayed as `clang++ --analyze` (deduplicated to one
analysis per source file).  See analyzer.pri.

Counts are deduplicated per (file:line:col, diagnostic) and
split into in-tree findings vs Qt/system-header noise.

## GCC -fanalyzer findings (353 in-tree, 107 in Qt/system headers)

```
== in-tree ==
src/.ui/ui_aboutdialog.h:103:45: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:110:40: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:120:42: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:130:44: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:136:52: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:141:52: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:146:50: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:47:53: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:49:38: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:61:38: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:77:41: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_aboutdialog.h:90:47: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:45:52: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:47:45: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:53:54: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:59:56: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:65:54: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:75:51: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:80:51: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:85:56: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_acceptlanguagedialog.h:92:48: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:41:59: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:43:45: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:50:46: warning: use of possibly-NULL ‘operator new(96)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:55:49: warning: use of possibly-NULL ‘operator new(96)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:60:51: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_addbookmarkdialog.h:69:59: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:43:52: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:49:51: warning: use of possibly-NULL ‘operator new(112)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:54:53: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:60:54: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:65:57: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_autofilldialog.h:74:56: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:45:53: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:51:52: warning: use of possibly-NULL ‘operator new(112)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:56:48: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:61:38: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:63:55: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:68:58: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_bookmarksdialog.h:77:57: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:46:51: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:52:50: warning: use of possibly-NULL ‘operator new(112)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:57:55: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:62:38: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:64:53: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:69:56: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:74:54: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookies.h:83:55: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:111:67: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:113:57: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:119:55: warning: use of possibly-NULL ‘operator new(112)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:124:62: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:129:58: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:134:61: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:146:65: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:60:61: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:62:69: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:64:58: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:66:38: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:68:48: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:73:60: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:81:39: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:87:59: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:93:69: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_cookiesexceptions.h:99:59: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:41:60: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:43:57: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:49:42: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:51:55: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:56:58: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:61:59: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_opensearchdialog.h:70:57: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:46:56: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:56:45: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:61:58: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:68:54: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:74:50: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:80:57: warning: use of possibly-NULL ‘operator new(112)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/.ui/ui_searchbanner.h:85:50: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/acceptlanguagedialog.cpp:101:94: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/acceptlanguagedialog.cpp:50:17: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/acceptlanguagedialog.cpp:94:28: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/acceptlanguagedialog.cpp:95:54: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockmanager.cpp:101:51: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:115:84: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:119:73: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:122:69: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:128:45: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:306:46: warning: use of possibly-NULL ‘operator new(136)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:58:37: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmanager.cpp:76:79: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockmodel.cpp:52:45: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockrule.cpp:491:40: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockrule.cpp:498:56: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockrule.cpp:601:29: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockschemeaccesshandler.cpp:46:23: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/adblock/adblockschemeaccesshandler.cpp:82:110: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/adblock/adblockschemeaccesshandler.cpp:82:80: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/bookmarks/addbookmarkdialog.cpp:138:26: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/addbookmarkdialog.cpp:148:23: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/addbookmarkdialog.cpp:206:51: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksdialog.cpp:243:63: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:113:44: warning: use of possibly-NULL ‘operator new(80)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:217:89: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:229:83: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:239:90: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:249:89: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:281:56: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmanager.cpp:95:37: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmenu.cpp:80:49: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarksmodel.cpp:152:34: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:153:36: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:156:69: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:170:51: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:171:36: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:176:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:177:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:181:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:184:30: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:187:35: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:190:64: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:195:78: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:198:76: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:291:17: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/bookmarks/bookmarksmodel.cpp:299:41: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/bookmarkstoolbar.cpp:146:49: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/bookmarks/xbel/xbelreader.cpp:213:77: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1021:40: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1026:39: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1075:55: warning: use of possibly-NULL ‘operator new(136)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1202:52: warning: use of possibly-NULL ‘operator new(152)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:124:37: warning: use of possibly-NULL ‘operator new(208)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:125:37: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1261:62: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1436:33: warning: leak of ‘<anonymous>.std::function<void(const QString&)>::<anonymous>.std::_Function_base::_M_functor.std::_Any_data::_M_unused.std::_Nocopy_types::_M_object’ [CWE-401] [-Wanalyzer-malloc-leak]
src/browsermainwindow.cpp:1436:45: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
src/browsermainwindow.cpp:1437:67: warning: use of possibly-NULL ‘operator new(136)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1532:43: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:1551:43: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browsermainwindow.cpp:309:16: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/browsermainwindow.cpp:459:76: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browserprofile.cpp:61:54: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/browserprofile.cpp:82:42: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/clearprivatedata.cpp:47:43: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:48:66: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:53:62: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:57:62: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:61:58: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:65:45: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:72:48: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:80:52: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:84:52: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:90:74: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:92:62: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/clearprivatedata.cpp:93:39: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/downloadmanager.cpp:1043:13: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/downloadmanager.cpp:554:47: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/downloadmanager.cpp:589:47: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/downloadmanager.cpp:622:44: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/downloadmanager.cpp:628:34: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/downloadmanager.cpp:638:52: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/extensions/extensionmanager.cpp:50:37: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/history/historycompleter.cpp:184:40: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/history/historycompleter.cpp:202:71: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/history/historycompleter.cpp:240:58: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/history/historycompleter.cpp:251:66: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/history/historycompleter.cpp:32:51: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/history/historycompleter.cpp:77:37: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/history/historycompleter.cpp:79:37: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/history/historycompleter.cpp:91:51: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/locationbar/locationbar.cpp:142:49: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/locationbar/locationbar.cpp:48:46: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/locationbar/locationbar.cpp:52:51: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/locationbar/locationbar.cpp:56:54: warning: use of possibly-NULL ‘operator new(64)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/locationbar/locationbar.cpp:97:36: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/locationbar/locationbarsiteicon.cpp:70:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/locationbar/locationbarsiteicon.cpp:71:35: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/main.cpp:1273:38: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
src/main.cpp:404:73: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/modelmenu.cpp:177:30: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/modelmenu.cpp:351:33: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/network/cookiejar/cookiemodel.cpp:101:29: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:103:31: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:105:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:107:33: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:112:70: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:126:34: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:128:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:130:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:132:36: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:134:42: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:136:33: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:145:34: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:147:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:149:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:151:63: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:153:103: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:155:33: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:162:16: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:88:35: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:97:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/cookiejar/cookiemodel.cpp:99:29: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/network/fileaccesshandler.cpp:126:29: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/network/fileaccesshandler.cpp:128:18: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/network/fileaccesshandler.cpp:132:18: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
src/network/networkproxyfactory.cpp:39:26: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchdialog.cpp:37:81: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/opensearch/opensearchenginemodel.cpp:113:33: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:118:81: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:119:20: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:139:93: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:142:76: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:152:37: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:155:80: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:157:57: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:204:25: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:206:29: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchenginemodel.cpp:208:32: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/opensearch/opensearchreader.cpp:100:53: warning: use of possibly-NULL ‘operator new(288)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/searchbar.cpp:32:41: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/searchbar.cpp:61:32: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/searchbutton.cpp:48:63: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/searchlineedit.cpp:43:43: warning: use of possibly-NULL ‘operator new(72)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/searchlineedit.cpp:47:41: warning: use of possibly-NULL ‘operator new(64)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/securestore.cpp:77:36: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/settings.cpp:779:67: warning: use of possibly-NULL ‘operator new(96)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/settings.cpp:807:74: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/settings.cpp:836:68: warning: use of possibly-NULL ‘operator new(96)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:129:19: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
src/sourceviewer.cpp:135:23: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
src/sourceviewer.cpp:148:19: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
src/sourceviewer.cpp:42:55: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:43:61: warning: use of possibly-NULL ‘operator new(80)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:44:65: warning: use of possibly-NULL ‘operator new(160)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:45:36: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:46:34: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:47:50: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/sourceviewer.cpp:48:55: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabbar.cpp:268:41: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabbar.cpp:269:39: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabwidget.cpp:373:36: warning: use of possibly-NULL ‘operator new(128)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabwidget.cpp:375:82: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabwidget.cpp:377:73: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabwidget.cpp:540:16: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/tabwidget.cpp:614:43: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/tabwidget.cpp:824:57: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/tabwidget.cpp:970:105: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:141:57: warning: use of possibly-NULL ‘operator new(88)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:350:82: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:368:81: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:398:62: warning: dereference of possibly-NULL ‘operator new(48)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
src/toolbarsearch.cpp:471:55: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:479:54: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:484:74: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/toolbarsearch.cpp:488:71: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/useragent/useragentmenu.cpp:112:47: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/useragent/useragentmenu.cpp:55:49: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/edittableview.cpp:42:51: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/edittreeview.cpp:41:51: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/lineedit.cpp:71:39: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/lineedit.cpp:73:48: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/lineedit.cpp:81:40: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/lineedit.cpp:83:50: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/utils/safetext.cpp:42:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/utils/singleapplication.cpp:161:27: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/utils/singleapplication.cpp:171:60: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/utils/singleapplication.cpp:84:42: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webview.cpp:104:39: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webview.cpp:161:33: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webview.cpp:291:48: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webview.cpp:450:12: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
src/webview.cpp:95:30: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webview.h:84:39: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
src/webview.h:99:42: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
src/webviewsearch.cpp:100:31: warning: use of possibly-NULL ‘operator new(32)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webviewsearch.cpp:103:56: warning: use of possibly-NULL ‘operator new(128)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
src/webviewsearch.cpp:80:24: warning: leak of ‘<anonymous>.std::function<void(const QWebEngineFindTextResult&)>::<anonymous>.std::_Function_base::_M_functor.std::_Any_data::_M_unused.std::_Nocopy_types::_M_object’ [CWE-401] [-Wanalyzer-malloc-leak]
tst_adblockmanager.cpp:147:81: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_adblockmanager.cpp:216:81: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_adblockpage.cpp:153:80: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_adblockrequestinterceptor.cpp:130:24: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_adblockrequestinterceptor.cpp:154:46: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_adblockrequestinterceptor.cpp:155:44: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_adblockrequestinterceptor.cpp:170:48: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_adblockrequestinterceptor.cpp:185:46: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_adblockrequestinterceptor.cpp:187:24: warning: leak of ‘<anonymous>.std::function<void(const QVariant&)>::<anonymous>.std::_Function_base::_M_functor.std::_Any_data::_M_unused.std::_Nocopy_types::_M_object’ [CWE-401] [-Wanalyzer-malloc-leak]
tst_adblockrequestinterceptor.cpp:188:38: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_adblockrequestinterceptor.cpp:193:13: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_adblockrequestinterceptor.cpp:224:61: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_autosaver.cpp:133:27: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_autosaver.cpp:145:27: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_autosaver.cpp:47:39: warning: dereference of possibly-NULL ‘operator new(40)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_bookmarknode.cpp:109:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:118:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:152:70: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:156:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:201:67: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:206:67: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:216:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:227:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:240:43: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:242:70: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarknode.cpp:98:66: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_bookmarksmodel.cpp:290:65: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_browsermainwindow.cpp:54:9: warning: dereference of possibly-NULL ‘operator new(608)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_certerror.cpp:143:48: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_certerror.cpp:214:48: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_cookiejar.cpp:86:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_cookiemodel.cpp:47:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_dialogs.cpp:353:69: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_dialogs.cpp:425:61: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_downloadmanager.cpp:235:44: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_downloadmanager.cpp:81:40: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_edittreeview.cpp:54:64: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_edittreeview.cpp:59:67: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_historyfiltermodel.cpp:111:17: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_historyfiltermodel.cpp:164:56: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_historyfiltermodel.cpp:61:42: warning: use of possibly-NULL ‘operator new(136)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_historyfiltermodel.cpp:62:54: warning: use of possibly-NULL ‘operator new(24)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_historyui.cpp:173:41: warning: use of possibly-NULL ‘operator new(56)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_lineedit.cpp:117:31: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_lineedit.cpp:136:31: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_modelmenu.cpp:111:67: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_modelmenu.cpp:176:69: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_modelmenu.cpp:243:61: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_modelmenu.cpp:274:69: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_modeltoolbar.cpp:157:45: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_networkcookiejar.cpp:66:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_opensearchmanager.cpp:127:53: warning: use of possibly-NULL ‘operator new(288)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_opensearchmanager.cpp:176:53: warning: use of possibly-NULL ‘operator new(288)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_opensearchmanager.cpp:228:53: warning: use of possibly-NULL ‘operator new(288)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_privatebrowsing.cpp:500:61: warning: use of possibly-NULL ‘operator new(216)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_schemehandlers.cpp:100:46: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_schemehandlers.cpp:101:44: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_schemehandlers.cpp:115:29: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_schemehandlers.cpp:63:40: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_settingsdialog.cpp:233:80: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_tabwidget.cpp:164:42: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:101:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:105:38: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:121:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:140:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:157:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:176:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:179:28: warning: use of possibly-NULL ‘operator new(120)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webactionmapper.cpp:199:37: warning: use of possibly-NULL ‘operator new(16)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
tst_webpermissions.cpp:200:17: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_webpermissions.cpp:79:48: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_webpermissions.cpp:84:22: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
tst_webview.cpp:285:24: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_webview.cpp:330:48: warning: dereference of possibly-NULL ‘operator new(1)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_webview.cpp:349:35: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
tst_webview.cpp:49:9: warning: dereference of possibly-NULL ‘operator new(120)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
== qt-headers ==
qt6/include/QtCore/qabstractitemmodel.h:519:47: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qarraydatapointer.h:33:11: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qarraydatapointer.h:39:19: warning: dereference of NULL ‘0’ [CWE-476] [-Wanalyzer-null-dereference]
qt6/include/QtCore/qarraydatapointer.h:46:11: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qarraydatapointer.h:79:11: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qbytearray.h:664:63: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qdebug.h:103:39: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qdebug.h:412:78: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qdebug.h:648:83: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qdir.h:235:55: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:1481:30: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:284:21: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QByteArray, {anonymous}::StubResource> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, OpenSearchEngine*> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, QByteArray> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, QHashDummyValue> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, QString> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, bool> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QString, int> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QTcpSocket*, QByteArray> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QWebEngineProfile*, CookieJar*> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<QWebEngineProfile*, QHashDummyValue> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:291:13: warning: use of uninitialized value ‘*this.QHashPrivate::Span<QHashPrivate::Node<std::pair<QString, QString>, QHashDummyValue> >::nextFree’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qhash.h:296:20: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:297:37: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:415:9: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 16)) + (i * 16))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 24)) + (i * 24))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 32)) + (i * 32))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 48)) + (i * 48))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 72)) + (i * 72))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:416:38: warning: dereference of possibly-NULL ‘(operator new []((alloc * 8)) + (i * 8))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:420:19: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:420:19: warning: leak of ‘operator new []((alloc * 24))’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:420:19: warning: leak of ‘operator new []((alloc * 32))’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:420:19: warning: leak of ‘operator new []((alloc * 72))’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:520:32: warning: use of NULL ‘<unknown>’ where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qhash.h:559:19: warning: leak of ‘QHashPrivate::Data<QHashPrivate::Node<QTcpSocket*, QByteArray> >::detached(*this.QHash<QTcpSocket*, QByteArray>::d)’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:559:19: warning: leak of ‘QHashPrivate::Data<QHashPrivate::Node<QTcpSocket*, QByteArray> >::detached(*this.QHash<QTcpSocket*, QByteArray>::d, (*this_25(D)->d.QHashPrivate::Data<QHashPrivate::Node<QTcpSocket*, QByteArray> >::size + 1))’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:559:44: warning: dereference of possibly-NULL ‘operator new [](((nSpans * 144) + 8))’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qhash.h:567:5: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:606:20: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:606:20: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
qt6/include/QtCore/qhash.h:607:20: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:607:20: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
qt6/include/QtCore/qhash.h:608:9: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:615:20: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:615:20: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
qt6/include/QtCore/qhash.h:616:20: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:616:20: warning: use of possibly-NULL ‘operator new(40)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
qt6/include/QtCore/qhash.h:674:5: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qhash.h:986:59: warning: leak of ‘<unknown>’ [CWE-401] [-Wanalyzer-malloc-leak]
qt6/include/QtCore/qlist.h:1074:22: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qlist.h:140:26: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qlist.h:215:26: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qlist.h:297:11: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qlist.h:751:53: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qlist.h:78:7: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qlist.h:917:49: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qmetacontainer.h:268:20: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetacontainer.h:274:67: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qmetacontainer.h:276:65: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qmetacontainer.h:347:20: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetacontainer.h:353:73: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qmetacontainer.h:355:71: warning: dereference of possibly-NULL ‘operator new(8)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qmetacontainer.h:430:20: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetacontainer.h:486:20: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetacontainer.h:499:24: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetacontainer.h:551:24: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qmetatype.h:2483:20: warning: use of NULL where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtCore/qobject.h:174:18: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qobject.h:275:78: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qobjectdefs_impl.h:445:45: warning: dereference of possibly-NULL ‘operator new(16)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(24)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(48)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(56)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(64)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:562:18: warning: dereference of possibly-NULL ‘operator new(96)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:563:18: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qobjectdefs_impl.h:619:16: warning: use of possibly-NULL ‘operator new(48)’ where non-null expected [CWE-690] [-Wanalyzer-possible-null-argument]
qt6/include/QtCore/qsharedpointer_impl.h:623:5: warning: dereference of possibly-NULL ‘operator new(16)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qsharedpointer_impl.h:626:5: warning: dereference of possibly-NULL ‘operator new(16)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qstring.h:1391:51: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:1424:53: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:1565:15: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:1738:70: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:381:62: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:513:54: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:566:34: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:754:43: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstring.h:776:86: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qstringlist.h:116:77: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qurl.h:155:35: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:1008:73: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:114:9: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:123:38: warning: dereference of possibly-NULL ‘operator new(32)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtCore/qvariant.h:958:7: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:988:71: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:993:77: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtCore/qvariant.h:995:31: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtGui/qaction.h:210:30: warning: use of NULL ‘custom’ where non-null expected [CWE-476] [-Wanalyzer-null-argument]
qt6/include/QtGui/qicon.h:191:11: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtGui/qtextformat.h:385:20: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtWidgets/qlayoutitem.h:63:9: warning: dereference of possibly-NULL ‘operator new(40)’ [CWE-690] [-Wanalyzer-possible-null-dereference]
qt6/include/QtWidgets/qplaintextedit.h:118:38: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
qt6/include/QtWidgets/qtreewidget.h:70:57: warning: use of uninitialized value ‘<unknown>’ [CWE-457] [-Wanalyzer-use-of-uninitialized-value]
```

## clang --analyze findings (6 in-tree, 0 in Qt/system headers)

```
== in-tree ==
src/bookmarks/xbel/xbelreader.cpp:188:7: warning: Potential leak of memory pointed to by 'folder' [cplusplus.NewDeleteLeaks]
src/network/cookiejar/cookiejar.cpp:102:22: warning: Forming reference to null pointer [core.NonNullParamChecker]
src/useragent/useragentmenu.cpp:90:5: warning: Potential leak of memory pointed to by 'actionGroup' [cplusplus.NewDeleteLeaks]
tst_adblockmanager.cpp:167:5: warning: Value stored to 'subscription' is never read [deadcode.DeadStores]
tst_dialogs.cpp:464:5: warning: Called C++ object pointer is null [core.CallAndMessage]
tst_downloadmanager.cpp:270:17: warning: Called C++ object pointer is null [core.CallAndMessage]
== qt-headers ==
```

## Triage (STAT01)

Every in-tree finding from both analyzers was examined individually.
**Zero actionable bugs were found.** All findings fall into five
false-positive families; each is justified below. The analyzers run
over the whole ported tree (src/, autotests/, tools/ — 294 unique
translation units on the clang pass, every TU on the GCC pass).

### GCC `-fanalyzer`

GCC 13's analyzer is not Qt-aware: it cannot see QObject parent
ownership, `QPointer` weak guards, `connect()` context-object
auto-disconnect, or implicitly-shared value semantics. That accounts
for every finding.

- **`use-of-uninitialized-value` (5,763 raw)** — the noisiest GCC 13
  checker. Every in-tree instance names the value `'<unknown>'` (the
  analyzer cannot attribute it — Qt implicitly-shared/QMetaType
  internals); every *named-variable* instance is inside Qt headers
  (`qhash.h`, `qmetacontainer.h`, `qarraydatapointer.h`, ...).
  Suppressed: no attributable in-tree site.

- **`possible-null-dereference` / `possible-null-argument` (1,092
  raw)** — all in-tree instances are `dereference of possibly-NULL
  'operator new(...)'` / `use of possibly-NULL 'operator new(...)'`.
  C++ `operator new` throws `std::bad_alloc` on failure and never
  returns null — GCC 13 models the non-throwing path anyway
  (documented FP, CWE-690). Suppressed.

- **`malloc-leak` (179 raw)** — the 3 unique in-tree sites are
  `std::function` type-erasure payloads handed to Qt async APIs:
  `browsermainwindow.cpp:1436` (`page->toHtml(callback)`),
  `webviewsearch.cpp:80` (`page->findText(..., callback)`),
  `tst_adblockrequestinterceptor.cpp:187` (WebEngine callback). Qt
  takes ownership of the callable and destroys it; the analyzer cannot
  see the sink. Suppressed.

- **`null-dereference` / `null-argument` (130 raw; 15 unique in-tree
  sites)** — all are the `QPointer`-guard pattern the analyzer cannot
  track through Qt's weak-pointer internals. Verified by hand:
  - `network/fileaccesshandler.cpp:126-132`,
    `adblock/adblockschemeaccesshandler.cpp:82` —
    `QPointer<QWebEngineUrlRequestJob> job`, guarded by
    `if (!job) return;` at function top.
  - `downloadmanager.cpp:554,589,622,628,638` — `m_download` QPointer,
    every use guarded (`m_download ?`, `if (m_download)`, `&&`).
  - `locationbar/locationbar.cpp:97,142` — `m_webView` guarded.
  - `sourceviewer.cpp:129,135,148` — `QPointer<SourceViewer> self`
    derefs: the lambdas are connected with `this` as the context
    object, so Qt drops them when the viewer dies; the `toHtml`
    callback (no context available) is explicitly `if (self)`-guarded.
  - `webview.h:84,99` — inlined `webPage()`/`progress()` bodies flagged
    at QPointer-guarded call sites (same guard-loss).

  Zero findings from the exploitable classes
  (`use-after-free`, `double-free`, `out-of-bounds`, `file`, `tainted`,
  `fd-*`, `unsafe-call-within-signal-handler`) anywhere in the build —
  those checkers were enabled and silent.

### clang `--analyze` (6 findings)

- `useragent/useragentmenu.cpp:90` `cplusplus.NewDeleteLeaks`
  `actionGroup` — `new QActionGroup(this)` is parented; Qt owns it. FP.
- `bookmarks/xbel/xbelreader.cpp:188` `cplusplus.NewDeleteLeaks`
  `folder` — `BookmarkNode(Folder, parent)`'s ctor calls
  `parent->add(this)`; ownership transfers into the node tree. FP.
- `network/cookiejar/cookiejar.cpp:102` `core.NonNullParamChecker` —
  `m_policy.block` is a plain `QStringList` member of a `this`-captured
  lambda; the member cannot be null and the jar is qApp-owned so it
  outlives the cookie store that invokes the filter (MIG03's QPointer
  design covers the opposite direction). FP.
- `autotests/dialogs/tst_dialogs.cpp:464` `core.CallAndMessage` —
  `custom->trigger()` sits behind `QVERIFY(custom)`; QVERIFY returns
  from the test on failure. FP.
- `autotests/downloadmanager/tst_downloadmanager.cpp:270`
  `core.CallAndMessage` — same QVERIFY guard pattern on
  `tryAgainButton`. FP.
- `autotests/adblock/adblockmanager/tst_adblockmanager.cpp:167`
  `deadcode.DeadStores` — the assigned value is intentionally unused;
  the `customRules()` call itself is the assertion (a second call must
  not re-emit signals). Benign.

### Environment notes

`clang-tidy`, `cppcheck`, and `scan-build` are not installed and cannot
be (no sudo, no network package installs) — noted per task
requirements. The sweep is therefore `g++ -fanalyzer` (GCC 13.3.0) and
`clang++ --analyze` (LLVM 18.x) only.

Hardening note for future runs: `make check-static` rebuilds pass 2
from scratch on every invocation (clang emits no reusable artifacts);
`ARORA_STATIC_BUILD=<dir>` resumes a pass-1 build after an
interruption, which matters here because a single `-fanalyzer` TU
peaks near 900 MB RSS and the task loop caps the tree at 2048 MB —
hence the `-j1` default for pass 1.
