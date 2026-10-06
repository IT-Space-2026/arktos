# Coverage baseline (COV01)

Generated: 2026-10-06 06:28 UTC on commit 211f551
Toolchain: Ubuntu clang version 18.1.3 (1ubuntu1) + /usr/bin/llvm-cov-18

## Reproduce

    source .devin/qt-env.sh && make check-coverage

Instrumented copy of the tree is built with `qmake -spec
linux-clang CONFIG+=coverage`, the autotest suite and the app
`--*-smoke` flags run under `LLVM_PROFILE_FILE`, and llvm-cov
reports over `src/` only (generated moc/ui/rcc and test drivers
excluded). `ARORA_COVERAGE_MIN=NN` turns it into a gate.

## Totals (src/ only)

| Metric | Coverage |
|--------|----------|
| Lines | 69.41% |
| Regions | 64.41% |
| Functions | 69.84% |

## Per-file

```
src/aboutdialog.cpp                                                                    12                12     0.00%           4                 4     0.00%          44                44     0.00%           2                 2     0.00%
src/acceptlanguagedialog.cpp                                                           60                38    36.67%          13                10    23.08%         131                97    25.95%          42                30    28.57%
src/adblock/adblockdialog.cpp                                                          27                25     7.41%           8                 7    12.50%          83                60    27.71%          14                14     0.00%
src/adblock/adblockmanager.cpp                                                         85                 8    90.59%          17                 0   100.00%         191                25    86.91%          50                10    80.00%
src/adblock/adblockmodel.cpp                                                          165                81    50.91%          15                 3    80.00%         202               112    44.55%         130                85    34.62%
src/adblock/adblocknetwork.cpp                                                        114                20    82.46%           5                 0   100.00%         135                14    89.63%         100                31    69.00%
src/adblock/adblocknetwork.h                                                            2                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/adblock/adblockpage.cpp                                                           227               108    52.42%           7                 2    71.43%         372               230    38.17%         172               113    34.30%
src/adblock/adblockrequestinterceptor.cpp                                              48                 2    95.83%           3                 0   100.00%          72                 4    94.44%          44                 5    88.64%
src/adblock/adblockresourcehandler.cpp                                                192                 0   100.00%          12                 0   100.00%         221                 0   100.00%          40                 3    92.50%
src/adblock/adblockrule.cpp                                                           311                12    96.14%          22                 1    95.45%         355                12    96.62%         256                22    91.41%
src/adblock/adblockrule.h                                                              13                 1    92.31%          13                 1    92.31%          13                 1    92.31%           0                 0         -
src/adblock/adblockschemeaccesshandler.cpp                                             20                 1    95.00%           7                 0   100.00%          46                 1    97.83%          10                 2    80.00%
src/adblock/adblocksubscription.cpp                                                   199                45    77.39%          27                 1    96.30%         270                26    90.37%          98                16    83.67%
src/autofilldialog.cpp                                                                 76                76     0.00%           8                 8     0.00%         125               125     0.00%          62                62     0.00%
src/autofillmanager.cpp                                                               118                31    73.73%          25                 1    96.00%         229                35    84.72%          68                29    57.35%
src/autofillmanager.h                                                                   1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/autosaver.cpp                                                                      47                26    44.68%           5                 0   100.00%          36                 9    75.00%          18                10    44.44%
src/bookmarks/addbookmarkdialog.cpp                                                    42                14    66.67%          14                 5    64.29%          99                31    68.69%          20                11    45.00%
src/bookmarks/bookmarknode.cpp                                                         54                 3    94.44%           9                 1    88.89%          59                 4    93.22%          34                 4    88.24%
src/bookmarks/bookmarksdialog.cpp                                                      56                56     0.00%          12                12     0.00%         143               143     0.00%          36                36     0.00%
src/bookmarks/bookmarksmanager.cpp                                                    191                55    71.20%          25                 3    88.00%         255                75    70.59%          96                48    50.00%
src/bookmarks/bookmarksmanager.h                                                        3                 1    66.67%           3                 1    66.67%           9                 3    66.67%           0                 0         -
src/bookmarks/bookmarksmenu.cpp                                                        45                36    20.00%           8                 5    37.50%          76                60    21.05%          24                22     8.33%
src/bookmarks/bookmarksmodel.cpp                                                      192                31    83.85%          20                 0   100.00%         260                47    81.92%         154                44    71.43%
src/bookmarks/bookmarksmodel.h                                                          1                 0   100.00%           1                 0   100.00%           3                 0   100.00%           0                 0         -
src/bookmarks/bookmarkstoolbar.cpp                                                     22                19    13.64%          10                 9    10.00%          88                75    14.77%           8                 8     0.00%
src/bookmarks/xbel/xbelreader.cpp                                                     115                44    61.74%          10                 2    80.00%         115                29    74.78%          84                44    47.62%
src/bookmarks/xbel/xbelwriter.cpp                                                      29                 2    93.10%           4                 0   100.00%          50                 3    94.00%          24                 4    83.33%
src/browserapplication.cpp                                                            179               113    36.87%          41                14    65.85%         375               196    47.73%         108                81    25.00%
src/browsermainwindow.cpp                                                             323               240    25.70%          74                53    28.38%        1158               478    58.72%         190               157    17.37%
src/browsermainwindow.h                                                                 1                 1     0.00%           1                 1     0.00%           1                 1     0.00%           0                 0         -
src/browserpaths.h                                                                      5                 1    80.00%           1                 0   100.00%          10                 1    90.00%           4                 1    75.00%
src/browserprofile.cpp                                                                 39                12    69.23%           6                 1    83.33%         112                14    87.50%          26                13    50.00%
src/clearbutton.cpp                                                                    18                 4    77.78%           3                 0   100.00%          44                 6    86.36%          10                 5    50.00%
src/clearprivatedata.cpp                                                               21                 2    90.48%           2                 0   100.00%          77                 2    97.40%          18                 8    55.56%
src/downloadmanager.cpp                                                               367                77    79.02%          51                 4    92.16%         655               142    78.32%         262                95    63.74%
src/extensions/extensionmanager.cpp                                                   251                24    90.44%          24                 1    95.83%         342                41    88.01%          94                37    60.64%
src/history/history.cpp                                                               469               158    66.31%          68                21    69.12%         730               220    69.86%         330               149    54.85%
src/history/history.h                                                                  13                 1    92.31%           5                 0   100.00%          12                 0   100.00%           8                 4    50.00%
src/history/historycompleter.cpp                                                       96                69    28.12%          18                11    38.89%         156               111    28.85%          62                53    14.52%
src/history/historymanager.cpp                                                        180                50    72.22%          26                 2    92.31%         274                35    87.23%         102                28    72.55%
src/history/historymanager.h                                                           11                 0   100.00%           4                 0   100.00%           7                 0   100.00%           6                 0   100.00%
src/locationbar/locationbar.cpp                                                        76                 7    90.79%          11                 0   100.00%         124                12    90.32%          56                18    67.86%
src/locationbar/locationbarsiteicon.cpp                                                21                11    47.62%           5                 1    80.00%          46                20    56.52%          16                13    18.75%
src/locationbar/privacyindicator.cpp                                                   10                 1    90.00%           3                 0   100.00%          14                 0   100.00%           6                 2    66.67%
src/main.cpp                                                                         1151               304    73.59%          70                14    80.00%        2478               176    92.90%         554               237    57.22%
src/modelmenu.cpp                                                                     106                95    10.38%          27                23    14.81%         205               184    10.24%          60                60     0.00%
src/modeltoolbar.cpp                                                                  101                53    47.52%          15                 5    66.67%         177                90    49.15%          64                46    28.12%
src/network/cookiejar/cookiedialog.cpp                                                 15                15     0.00%           2                 2     0.00%          57                57     0.00%          12                12     0.00%
src/network/cookiejar/cookieexceptionsdialog.cpp                                       15                15     0.00%           7                 7     0.00%          71                71     0.00%           8                 8     0.00%
src/network/cookiejar/cookieexceptionsmodel.cpp                                        80                 3    96.25%           8                 0   100.00%         134                 2    98.51%          82                14    82.93%
src/network/cookiejar/cookiejar.cpp                                                   214                58    72.90%          33                 5    84.85%         348                69    80.17%         190                87    54.21%
src/network/cookiejar/cookiemodel.cpp                                                  76                10    86.84%           7                 0   100.00%         119                10    91.60%          80                12    85.00%
src/network/cookiejar/networkcookiejar/networkcookiejar.cpp                           138                 9    93.48%          16                 0   100.00%         200                10    95.00%         110                20    81.82%
src/network/cookiejar/networkcookiejar/networkcookiejar_p.h                             6                 0   100.00%           3                 0   100.00%          14                 0   100.00%           2                 1    50.00%
src/network/cookiejar/networkcookiejar/trie_p.h                                        70                12    82.86%          11                 0   100.00%         112                 3    97.32%          44                21    52.27%
src/network/fileaccesshandler.cpp                                                      50                 5    90.00%           8                 0   100.00%         112                15    86.61%          32                 6    81.25%
src/network/networkaccessmanager.cpp                                                   77                58    24.68%          10                 5    50.00%         206               140    32.04%          54                46    14.81%
src/network/networkdiskcache.cpp                                                        9                 2    77.78%           4                 1    75.00%          22                 4    81.82%           2                 1    50.00%
src/network/networkproxyfactory.cpp                                                     9                 1    88.89%           4                 0   100.00%          16                 1    93.75%           6                 2    66.67%
src/network/schemeaccesshandler.cpp                                                     4                 0   100.00%           3                 0   100.00%          17                 0   100.00%           0                 0         -
src/opensearch/opensearchdialog.cpp                                                    12                12     0.00%           4                 4     0.00%          42                42     0.00%           6                 6     0.00%
src/opensearch/opensearchengine.cpp                                                   143                 9    93.71%          37                 0   100.00%         239                 6    97.49%          86                21    75.58%
src/opensearch/opensearchengineaction.cpp                                               9                 9     0.00%           2                 2     0.00%          16                16     0.00%           4                 4     0.00%
src/opensearch/opensearchenginedelegate.cpp                                             2                 0   100.00%           2                 0   100.00%           4                 0   100.00%           0                 0         -
src/opensearch/opensearchenginemodel.cpp                                               76                76     0.00%           9                 9     0.00%         106               106     0.00%          66                66     0.00%
src/opensearch/opensearchmanager.cpp                                                  159                44    72.33%          28                 4    85.71%         259                63    75.68%          98                34    65.31%
src/opensearch/opensearchreader.cpp                                                    85                 2    97.65%           3                 0   100.00%          76                 2    97.37%          84                12    85.71%
src/opensearch/opensearchwriter.cpp                                                    82                 2    97.56%           3                 0   100.00%          64                 2    96.88%          24                 5    79.17%
src/plaintexteditsearch.cpp                                                            17                 2    88.24%           4                 1    75.00%          34                 4    88.24%          10                 4    60.00%
src/searchbar.cpp                                                                      26                14    46.15%           9                 5    44.44%          77                33    57.14%          10                 8    20.00%
src/searchbutton.cpp                                                                   26                 5    80.77%           7                 2    71.43%          68                11    83.82%          12                 4    66.67%
src/searchlineedit.cpp                                                                  5                 0   100.00%           4                 0   100.00%          23                 0   100.00%           0                 0         -
src/settings.cpp                                                                      218               157    27.98%          26                18    30.77%         557               263    52.78%         208               138    33.65%
src/sourcehighlighter.cpp                                                              55                13    76.36%           4                 1    75.00%         135                16    88.15%          44                14    68.18%
src/sourceviewer.cpp                                                                   33                 7    78.79%           6                 1    83.33%         120                13    89.17%          14                 8    42.86%
src/tabbar.cpp                                                                        106                74    30.19%          25                14    44.00%         206               139    32.52%          72                59    18.06%
src/tabwidget.cpp                                                                     394               196    50.25%          63                28    55.56%         738               269    63.55%         268               166    38.06%
src/tabwidget.h                                                                         1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/toolbarsearch.cpp                                                                 157               121    22.93%          22                14    36.36%         286               199    30.42%         106                95    10.38%
src/useragent/useragentmenu.cpp                                                        39                37     5.13%           6                 5    16.67%          83                79     4.82%          28                28     0.00%
src/utils/edittableview.cpp                                                            28                26     7.14%           4                 3    25.00%          33                30     9.09%          18                18     0.00%
src/utils/edittreeview.cpp                                                             23                 0   100.00%           4                 0   100.00%          25                 0   100.00%          14                 2    85.71%
src/utils/languagemanager.cpp                                                         100                27    73.00%          12                 2    83.33%         181                59    67.40%          62                20    67.74%
src/utils/lineedit.cpp                                                                 69                16    76.81%          17                 1    94.12%         152                19    87.50%          36                 9    75.00%
src/utils/singleapplication.cpp                                                        56                53     5.36%           6                 5    16.67%          79                75     5.06%          30                30     0.00%
src/utils/squeezelabel.cpp                                                              7                 0   100.00%           2                 0   100.00%          14                 0   100.00%           4                 0   100.00%
src/utils/treesortfilterproxymodel.cpp                                                  6                 0   100.00%           2                 0   100.00%          10                 0   100.00%           2                 0   100.00%
src/webactionmapper.cpp                                                                39                 1    97.44%           8                 0   100.00%          61                 1    98.36%          22                 4    81.82%
src/webpage.cpp                                                                       130                24    81.54%          23                 4    82.61%         246                35    85.77%          74                26    64.86%
src/webview.cpp                                                                       195               114    41.54%          37                13    64.86%         339               170    49.85%         136                98    27.94%
src/webview.h                                                                           2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/webviewsearch.cpp                                                                  27                 2    92.59%           8                 0   100.00%          59                 2    96.61%          12                 4    66.67%
TOTAL regions 64.41% (5794/8995)  functions 69.84% (887/1270)  lines 69.41% (11414/16444)
```
