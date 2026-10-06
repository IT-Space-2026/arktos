# Coverage baseline (COV01)

Generated: 2026-10-06 08:30 UTC on commit f67f641+working-tree
Toolchain: Ubuntu clang version 18.1.3 (1ubuntu1) + /usr/bin/llvm-cov-18

## Reproduce

    source .devin/qt-env.sh && make check-coverage

Instrumented copy of the tree is built with `qmake -spec
linux-clang CONFIG+=coverage`, the autotest suite and the app
`--*-smoke` flags run under `LLVM_PROFILE_FILE`, and llvm-cov
reports over `src/` only (generated moc/ui/rcc and test drivers
excluded). `ARORA_COVERAGE_MIN=NN` turns it into a gate
(default 80).

## Totals (src/ only)

| Metric | Coverage |
|--------|----------|
| Lines | 80.47% |
| Regions | 73.92% |
| Functions | 85.18% |

## Per-file

```
src/aboutdialog.cpp                                                                    12                 1    91.67%           4                 0   100.00%          44                 1    97.73%           2                 1    50.00%
src/acceptlanguagedialog.cpp                                                           60                 7    88.33%          13                 2    84.62%         131                21    83.97%          42                13    69.05%
src/adblock/adblockdialog.cpp                                                          27                16    40.74%           8                 5    37.50%          83                27    67.47%          14                12    14.29%
src/adblock/adblockmanager.cpp                                                         85                 8    90.59%          17                 0   100.00%         191                25    86.91%          50                10    80.00%
src/adblock/adblockmodel.cpp                                                          165                65    60.61%          15                 2    86.67%         202                93    53.96%         130                75    42.31%
src/adblock/adblocknetwork.cpp                                                        114                20    82.46%           5                 0   100.00%         135                14    89.63%         100                31    69.00%
src/adblock/adblocknetwork.h                                                            2                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/adblock/adblockpage.cpp                                                           227               103    54.63%           7                 2    71.43%         372               227    38.98%         172               105    38.95%
src/adblock/adblockrequestinterceptor.cpp                                              48                 2    95.83%           3                 0   100.00%          72                 4    94.44%          44                 5    88.64%
src/adblock/adblockresourcehandler.cpp                                                192                 0   100.00%          12                 0   100.00%         221                 0   100.00%          40                 3    92.50%
src/adblock/adblockrule.cpp                                                           311                12    96.14%          22                 1    95.45%         355                12    96.62%         256                22    91.41%
src/adblock/adblockrule.h                                                              13                 1    92.31%          13                 1    92.31%          13                 1    92.31%           0                 0         -
src/adblock/adblockschemeaccesshandler.cpp                                             20                 1    95.00%           7                 0   100.00%          46                 1    97.83%          10                 2    80.00%
src/adblock/adblocksubscription.cpp                                                   199                45    77.39%          27                 1    96.30%         270                26    90.37%          98                16    83.67%
src/autofilldialog.cpp                                                                 76                25    67.11%           8                 0   100.00%         125                16    87.20%          62                27    56.45%
src/autofillmanager.cpp                                                               118                31    73.73%          25                 1    96.00%         229                35    84.72%          68                29    57.35%
src/autofillmanager.h                                                                   1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/autosaver.cpp                                                                      47                26    44.68%           5                 0   100.00%          36                 9    75.00%          18                10    44.44%
src/bookmarks/addbookmarkdialog.cpp                                                    42                 6    85.71%          14                 3    78.57%          99                13    86.87%          20                 5    75.00%
src/bookmarks/bookmarknode.cpp                                                         54                 3    94.44%           9                 1    88.89%          59                 4    93.22%          34                 4    88.24%
src/bookmarks/bookmarksdialog.cpp                                                      56                34    39.29%          12                 8    33.33%         143                75    47.55%          36                26    27.78%
src/bookmarks/bookmarksmanager.cpp                                                    191                55    71.20%          25                 3    88.00%         255                75    70.59%          96                48    50.00%
src/bookmarks/bookmarksmanager.h                                                        3                 1    66.67%           3                 1    66.67%           9                 3    66.67%           0                 0         -
src/bookmarks/bookmarksmenu.cpp                                                        45                18    60.00%           8                 1    87.50%          76                19    75.00%          24                13    45.83%
src/bookmarks/bookmarksmodel.cpp                                                      192                31    83.85%          20                 0   100.00%         260                47    81.92%         154                44    71.43%
src/bookmarks/bookmarksmodel.h                                                          1                 0   100.00%           1                 0   100.00%           3                 0   100.00%           0                 0         -
src/bookmarks/bookmarkstoolbar.cpp                                                     22                16    27.27%          10                 8    20.00%          88                69    21.59%           8                 8     0.00%
src/bookmarks/xbel/xbelreader.cpp                                                     115                44    61.74%          10                 2    80.00%         115                29    74.78%          84                44    47.62%
src/bookmarks/xbel/xbelwriter.cpp                                                      29                 2    93.10%           4                 0   100.00%          50                 3    94.00%          24                 4    83.33%
src/browserapplication.cpp                                                            179                64    64.25%          41                 5    87.80%         376               116    69.15%         108                57    47.22%
src/browsermainwindow.cpp                                                             323                99    69.35%          74                11    85.14%        1158               167    85.58%         190                89    53.16%
src/browsermainwindow.h                                                                 1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/browserpaths.h                                                                      5                 2    60.00%           1                 0   100.00%          10                 4    60.00%           4                 2    50.00%
src/browserprofile.cpp                                                                 44                 4    90.91%           8                 0   100.00%         131                 7    94.66%          28                 7    75.00%
src/clearbutton.cpp                                                                    18                 4    77.78%           3                 0   100.00%          44                 6    86.36%          10                 5    50.00%
src/clearprivatedata.cpp                                                               21                 0   100.00%           2                 0   100.00%          77                 0   100.00%          18                 6    66.67%
src/downloadmanager.cpp                                                               367                76    79.29%          51                 4    92.16%         655               141    78.47%         262                93    64.50%
src/extensions/extensionmanager.cpp                                                   251                23    90.84%          24                 1    95.83%         342                40    88.30%          94                36    61.70%
src/history/history.cpp                                                               469                84    82.09%          68                 6    91.18%         730                99    86.44%         330               113    65.76%
src/history/history.h                                                                  13                 1    92.31%           5                 0   100.00%          12                 0   100.00%           8                 4    50.00%
src/history/historycompleter.cpp                                                       96                41    57.29%          18                 4    77.78%         156                61    60.90%          62                40    35.48%
src/history/historymanager.cpp                                                        180                47    73.89%          26                 2    92.31%         274                31    88.69%         102                27    73.53%
src/history/historymanager.h                                                           11                 0   100.00%           4                 0   100.00%           7                 0   100.00%           6                 0   100.00%
src/locationbar/locationbar.cpp                                                        76                 7    90.79%          11                 0   100.00%         124                12    90.32%          56                18    67.86%
src/locationbar/locationbarsiteicon.cpp                                                21                11    47.62%           5                 1    80.00%          46                20    56.52%          16                13    18.75%
src/locationbar/privacyindicator.cpp                                                   10                 1    90.00%           3                 0   100.00%          14                 0   100.00%           6                 2    66.67%
src/main.cpp                                                                         1216               326    73.19%          73                15    79.45%        2584               186    92.80%         590               252    57.29%
src/modelmenu.cpp                                                                     106                43    59.43%          27                 5    81.48%         205                77    62.44%          60                34    43.33%
src/modeltoolbar.cpp                                                                  101                53    47.52%          15                 5    66.67%         177                90    49.15%          64                46    28.12%
src/network/cookiejar/cookiedialog.cpp                                                 15                 1    93.33%           2                 0   100.00%          57                 1    98.25%          12                 1    91.67%
src/network/cookiejar/cookieexceptionsdialog.cpp                                       15                 0   100.00%           7                 0   100.00%          71                 0   100.00%           8                 1    87.50%
src/network/cookiejar/cookieexceptionsmodel.cpp                                        80                 2    97.50%           8                 0   100.00%         134                 1    99.25%          82                13    84.15%
src/network/cookiejar/cookiejar.cpp                                                   214                58    72.90%          33                 5    84.85%         348                69    80.17%         190                87    54.21%
src/network/cookiejar/cookiemodel.cpp                                                  76                 9    88.16%           7                 0   100.00%         119                 9    92.44%          80                11    86.25%
src/network/cookiejar/networkcookiejar/networkcookiejar.cpp                           138                 9    93.48%          16                 0   100.00%         200                10    95.00%         110                19    82.73%
src/network/cookiejar/networkcookiejar/networkcookiejar_p.h                             6                 0   100.00%           3                 0   100.00%          14                 0   100.00%           2                 1    50.00%
src/network/cookiejar/networkcookiejar/trie_p.h                                        70                12    82.86%          11                 0   100.00%         112                 3    97.32%          44                21    52.27%
src/network/fileaccesshandler.cpp                                                      50                 5    90.00%           8                 0   100.00%         112                15    86.61%          32                 6    81.25%
src/network/networkaccessmanager.cpp                                                   77                34    55.84%          10                 2    80.00%         206                82    60.19%          54                31    42.59%
src/network/networkdiskcache.cpp                                                        9                 1    88.89%           4                 0   100.00%          22                 1    95.45%           2                 1    50.00%
src/network/networkproxyfactory.cpp                                                     9                 0   100.00%           4                 0   100.00%          16                 0   100.00%           6                 0   100.00%
src/network/schemeaccesshandler.cpp                                                     4                 0   100.00%           3                 0   100.00%          17                 0   100.00%           0                 0         -
src/opensearch/opensearchdialog.cpp                                                    12                 3    75.00%           4                 0   100.00%          42                 5    88.10%           6                 3    50.00%
src/opensearch/opensearchengine.cpp                                                   143                 9    93.71%          37                 0   100.00%         239                 6    97.49%          86                20    76.74%
src/opensearch/opensearchengineaction.cpp                                               9                 0   100.00%           2                 0   100.00%          16                 0   100.00%           4                 1    75.00%
src/opensearch/opensearchenginedelegate.cpp                                             2                 0   100.00%           2                 0   100.00%           4                 0   100.00%           0                 0         -
src/opensearch/opensearchenginemodel.cpp                                               79                17    78.48%           9                 0   100.00%         108                23    78.70%          68                23    66.18%
src/opensearch/opensearchmanager.cpp                                                  159                44    72.33%          28                 4    85.71%         259                64    75.29%          98                34    65.31%
src/opensearch/opensearchreader.cpp                                                    85                 2    97.65%           3                 0   100.00%          76                 2    97.37%          84                12    85.71%
src/opensearch/opensearchwriter.cpp                                                    82                 2    97.56%           3                 0   100.00%          64                 2    96.88%          24                 5    79.17%
src/plaintexteditsearch.cpp                                                            17                 2    88.24%           4                 1    75.00%          34                 4    88.24%          10                 4    60.00%
src/searchbar.cpp                                                                      26                 3    88.46%           9                 0   100.00%          77                 1    98.70%          10                 4    60.00%
src/searchbutton.cpp                                                                   26                 3    88.46%           7                 2    71.43%          68                 9    86.76%          12                 1    91.67%
src/searchlineedit.cpp                                                                  5                 0   100.00%           4                 0   100.00%          23                 0   100.00%           0                 0         -
src/settings.cpp                                                                      218               121    44.50%          26                 6    76.92%         557               180    67.68%         208               110    47.12%
src/sourcehighlighter.cpp                                                              55                13    76.36%           4                 1    75.00%         135                16    88.15%          44                14    68.18%
src/sourceviewer.cpp                                                                   33                 7    78.79%           6                 1    83.33%         120                13    89.17%          14                 8    42.86%
src/tabbar.cpp                                                                        106                74    30.19%          25                14    44.00%         206               139    32.52%          72                59    18.06%
src/tabwidget.cpp                                                                     394               172    56.35%          63                23    63.49%         738               234    68.29%         268               153    42.91%
src/tabwidget.h                                                                         1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/toolbarsearch.cpp                                                                 157                80    49.04%          22                 9    59.09%         286               126    55.94%         106                72    32.08%
src/useragent/useragentmenu.cpp                                                        39                10    74.36%           6                 0   100.00%          83                 6    92.77%          28                 5    82.14%
src/utils/edittableview.cpp                                                            28                 3    89.29%           4                 0   100.00%          33                 4    87.88%          18                 6    66.67%
src/utils/edittreeview.cpp                                                             23                 0   100.00%           4                 0   100.00%          25                 0   100.00%          14                 2    85.71%
src/utils/languagemanager.cpp                                                         100                27    73.00%          12                 2    83.33%         181                59    67.40%          62                20    67.74%
src/utils/lineedit.cpp                                                                 69                16    76.81%          17                 1    94.12%         152                19    87.50%          36                 8    77.78%
src/utils/singleapplication.cpp                                                        56                28    50.00%           6                 0   100.00%          79                22    72.15%          30                18    40.00%
src/utils/squeezelabel.cpp                                                              7                 0   100.00%           2                 0   100.00%          14                 0   100.00%           4                 0   100.00%
src/utils/treesortfilterproxymodel.cpp                                                  6                 0   100.00%           2                 0   100.00%          10                 0   100.00%           2                 0   100.00%
src/webactionmapper.cpp                                                                39                 1    97.44%           8                 0   100.00%          61                 1    98.36%          22                 4    81.82%
src/webpage.cpp                                                                       135                24    82.22%          23                 4    82.61%         251                35    86.06%          78                26    66.67%
src/webview.cpp                                                                       195               114    41.54%          37                13    64.86%         339               170    49.85%         136                98    27.94%
src/webview.h                                                                           2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/webviewsearch.cpp                                                                  27                 1    96.30%           8                 0   100.00%          59                 1    98.31%          12                 3    75.00%
TOTAL regions 73.92% (6707/9073)  functions 85.18% (1086/1275)  lines 80.47% (13339/16577)
```
