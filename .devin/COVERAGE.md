# Coverage baseline (COV01)

Generated: 2026-10-11 00:12 UTC on commit 264f413
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
| Lines | 82.57% |
| Regions | 76.38% |
| Functions | 86.79% |

## Per-file

```
src/aboutdialog.cpp                                                                    12                 1    91.67%           4                 0   100.00%          44                 1    97.73%           2                 1    50.00%
src/acceptlanguagedialog.cpp                                                           70                 7    90.00%          14                 2    85.71%         144                21    85.42%          52                15    71.15%
src/adblock/adblockdialog.cpp                                                          92                15    83.70%          23                 5    78.26%         244                28    88.52%          56                14    75.00%
src/adblock/adblockmanager.cpp                                                        198                31    84.34%          28                 1    96.43%         364                59    83.79%         140                37    73.57%
src/adblock/adblocknetwork.cpp                                                        313                16    94.89%          15                 0   100.00%         398                16    95.98%         262                36    86.26%
src/adblock/adblocknetwork.h                                                            2                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/adblock/adblockpage.cpp                                                           279               109    60.93%          12                 2    83.33%         445               227    48.99%         206               102    50.49%
src/adblock/adblockpresets.cpp                                                          2                 0   100.00%           2                 0   100.00%          88                 0   100.00%           0                 0         -
src/adblock/adblockpresetsdialog.cpp                                                   16                 2    87.50%           2                 0   100.00%          70                 2    97.14%          14                 4    71.43%
src/adblock/adblockrequestinterceptor.cpp                                              83                20    75.90%           7                 1    85.71%         106                16    84.91%          64                13    79.69%
src/adblock/adblockresourcehandler.cpp                                                192                 0   100.00%          12                 0   100.00%         220                 0   100.00%          40                 3    92.50%
src/adblock/adblockrule.cpp                                                           461                17    96.31%          25                 1    96.00%         512                16    96.88%         414                42    89.86%
src/adblock/adblockrule.h                                                              14                 0   100.00%          14                 0   100.00%          14                 0   100.00%           0                 0         -
src/adblock/adblockrustengine.cpp                                                      75                14    81.33%           6                 0   100.00%          99                12    87.88%          62                15    75.81%
src/adblock/adblockrustengine.h                                                         1                 1     0.00%           1                 1     0.00%           1                 1     0.00%           0                 0         -
src/adblock/adblockschemeaccesshandler.cpp                                             20                 1    95.00%           7                 0   100.00%          51                 1    98.04%          10                 2    80.00%
src/adblock/adblocksubscription.cpp                                                   238                56    76.47%          30                 1    96.67%         297                33    88.89%         118                18    84.75%
src/argon2id.c                                                                        259                 8    96.91%          18                 0   100.00%         379                 9    97.63%         130                16    87.69%
src/autofilldialog.cpp                                                                 76                25    67.11%           8                 0   100.00%         125                16    87.20%          62                27    56.45%
src/autofillmanager.cpp                                                               226                68    69.91%          33                 1    96.97%         380                45    88.16%         136                45    66.91%
src/autofillmanager.h                                                                   1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/autosaver.cpp                                                                      47                 9    80.85%           5                 0   100.00%          36                 5    86.11%          18                 7    61.11%
src/bookmarks/addbookmarkdialog.cpp                                                    42                 5    88.10%          14                 3    78.57%         100                13    87.00%          20                 3    85.00%
src/bookmarks/bookmarknode.cpp                                                        105                 9    91.43%          13                 1    92.31%         142                 9    93.66%          74                13    82.43%
src/bookmarks/bookmarknode.h                                                            3                 0   100.00%           3                 0   100.00%           3                 0   100.00%           0                 0         -
src/bookmarks/bookmarksdialog.cpp                                                      56                34    39.29%          12                 8    33.33%         143                79    44.76%          36                27    25.00%
src/bookmarks/bookmarksmanager.cpp                                                    224                61    72.77%          29                 2    93.10%         345                90    73.91%         124                59    52.42%
src/bookmarks/bookmarksmanager.h                                                        3                 1    66.67%           3                 1    66.67%           9                 3    66.67%           0                 0         -
src/bookmarks/bookmarksmenu.cpp                                                        45                18    60.00%           8                 1    87.50%          76                19    75.00%          24                13    45.83%
src/bookmarks/bookmarksmodel.cpp                                                      192                31    83.85%          20                 0   100.00%         260                47    81.92%         154                43    72.08%
src/bookmarks/bookmarksmodel.h                                                          1                 0   100.00%           1                 0   100.00%           3                 0   100.00%           0                 0         -
src/bookmarks/bookmarkstoolbar.cpp                                                     22                16    27.27%          10                 8    20.00%          88                69    21.59%           8                 8     0.00%
src/bookmarks/xbel/xbelreader.cpp                                                     140                53    62.14%          10                 2    80.00%         154                48    68.83%         102                55    46.08%
src/bookmarks/xbel/xbelwriter.cpp                                                      29                 2    93.10%           4                 0   100.00%          50                 3    94.00%          24                 4    83.33%
src/browserapplication.cpp                                                            328               152    53.66%          54                12    77.78%         649               232    64.25%         206               117    43.20%
src/browsermainwindow.cpp                                                             418               116    72.25%          99                22    77.78%        1455               240    83.51%         258               118    54.26%
src/browserpaths.h                                                                      5                 1    80.00%           1                 0   100.00%          10                 1    90.00%           4                 1    75.00%
src/browserprofile.cpp                                                                239                44    81.59%          33                 1    96.97%         540                44    91.85%         166                37    77.71%
src/browsertheme.cpp                                                                   65                 4    93.85%           9                 0   100.00%         133                 4    96.99%          48                12    75.00%
src/clearbutton.cpp                                                                    14                 5    64.29%           3                 0   100.00%          40                14    65.00%           6                 4    33.33%
src/clearprivatedata.cpp                                                              185                 7    96.22%           9                 0   100.00%         279                 7    97.49%         158                24    84.81%
src/commandpalette.cpp                                                                314                43    86.31%          35                 5    85.71%         515                63    87.77%         194                57    70.62%
src/containermanager.cpp                                                              249                23    90.76%          37                 0   100.00%         390                22    94.36%         178                35    80.34%
src/devtoolswindow.cpp                                                                 28                 1    96.43%           5                 0   100.00%          52                 1    98.08%          18                 3    83.33%
src/devtoolswindow.h                                                                    2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/downloadmanager.cpp                                                               681                96    85.90%          65                 2    96.92%        1033               166    83.93%         432               127    70.60%
src/downloadmanager.h                                                                   1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/extensions/extensionmanager.cpp                                                   483               219    54.66%          49                 8    83.67%         889               222    75.03%         270               116    57.04%
src/extensions/extensionreviewdialog.cpp                                              114                 4    96.49%           5                 0   100.00%         192                 7    96.35%          62                 9    85.48%
src/history/history.cpp                                                               469                75    84.01%          68                 6    91.18%         730                93    87.26%         330               104    68.48%
src/history/history.h                                                                  13                 1    92.31%           5                 0   100.00%          12                 0   100.00%           8                 4    50.00%
src/history/historycompleter.cpp                                                      359                75    79.11%          59                 7    88.14%         587                95    83.82%         284                94    66.90%
src/history/historymanager.cpp                                                        211                66    68.72%          31                 1    96.77%         372                56    84.95%         122                39    68.03%
src/history/historymanager.h                                                           11                 0   100.00%           4                 0   100.00%           7                 0   100.00%           6                 2    66.67%
src/history/historyparser.cpp                                                          34                12    64.71%           2                 0   100.00%          52                10    80.77%          26                15    42.31%
src/locationbar/adblockbutton.cpp                                                      68                19    72.06%          13                 1    92.31%         152                37    75.66%          38                16    57.89%
src/locationbar/locationbar.cpp                                                       165                 7    95.76%          15                 0   100.00%         284                 7    97.54%         130                21    83.85%
src/locationbar/locationbarsiteicon.cpp                                                21                11    47.62%           5                 1    80.00%          50                20    60.00%          16                13    18.75%
src/locationbar/omniboxsuggestions.cpp                                                 69                 7    89.86%           7                 0   100.00%          86                 6    93.02%          52                14    73.08%
src/locationbar/popupblockerbutton.cpp                                                 68                47    30.88%          10                 5    50.00%         134                92    31.34%          40                33    17.50%
src/locationbar/privacyindicator.cpp                                                    9                 1    88.89%           3                 0   100.00%          22                 0   100.00%           6                 2    66.67%
src/locationbar/readerbutton.cpp                                                       39                12    69.23%           7                 3    57.14%          71                12    83.10%          30                17    43.33%
src/locationbar/sitepanel.cpp                                                         208                62    70.19%          21                 4    80.95%         414                79    80.92%         162                62    61.73%
src/locationbar/siteshield.cpp                                                         25                 4    84.00%           5                 0   100.00%          77                15    80.52%          14                 5    64.29%
src/locationbar/siteshield.h                                                            1                 1     0.00%           1                 1     0.00%           1                 1     0.00%           0                 0         -
src/main.cpp                                                                         5448              1778    67.36%         371                85    77.09%       14672              2832    80.70%        2622              1143    56.41%
src/modelmenu.cpp                                                                     134                48    64.18%          28                 5    82.14%         258                84    67.44%          82                43    47.56%
src/modeltoolbar.cpp                                                                  101                53    47.52%          15                 5    66.67%         178                90    49.44%          64                46    28.12%
src/network/cookiejar/cookiedialog.cpp                                                 15                 1    93.33%           2                 0   100.00%          57                 1    98.25%          12                 1    91.67%
src/network/cookiejar/cookieexceptionsdialog.cpp                                       15                 0   100.00%           7                 0   100.00%          71                 0   100.00%           8                 1    87.50%
src/network/cookiejar/cookieexceptionsmodel.cpp                                        80                 2    97.50%           8                 0   100.00%         134                 1    99.25%          82                13    84.15%
src/network/cookiejar/cookiejar.cpp                                                   320                35    89.06%          40                 2    95.00%         496                47    90.52%         298                88    70.47%
src/network/cookiejar/cookiemodel.cpp                                                  76                 9    88.16%           7                 0   100.00%         119                 9    92.44%          80                11    86.25%
src/network/cookiejar/networkcookiejar/networkcookiejar.cpp                           138                 9    93.48%          16                 0   100.00%         200                10    95.00%         110                19    82.73%
src/network/cookiejar/networkcookiejar/networkcookiejar_p.h                             6                 0   100.00%           3                 0   100.00%          14                 0   100.00%           2                 1    50.00%
src/network/cookiejar/networkcookiejar/trie_p.h                                        71                11    84.51%          11                 0   100.00%         116                 3    97.41%          46                20    56.52%
src/network/domainblocklist.cpp                                                        91                36    60.44%          11                 0   100.00%         123                19    84.55%          50                17    66.00%
src/network/fileaccesshandler.cpp                                                      59                 7    88.14%           8                 0   100.00%         120                18    85.00%          42                11    73.81%
src/network/networkaccessmanager.cpp                                                   79                34    56.96%          10                 2    80.00%         207                82    60.39%          58                31    46.55%
src/network/networkdiskcache.cpp                                                        9                 1    88.89%           4                 0   100.00%          22                 1    95.45%           2                 1    50.00%
src/network/networkproxyfactory.cpp                                                     9                 0   100.00%           4                 0   100.00%          16                 0   100.00%           6                 0   100.00%
src/network/privacyrequestinterceptor.cpp                                             476                38    92.02%          53                 3    94.34%         671                50    92.55%         414                91    78.02%
src/network/schemeaccesshandler.cpp                                                    34                 3    91.18%          12                 0   100.00%          96                 5    94.79%          12                 5    58.33%
src/network/tlsverifier.cpp                                                            67                 4    94.03%          12                 0   100.00%         148                 4    97.30%          30                 7    76.67%
src/opensearch/opensearchdialog.cpp                                                    12                 3    75.00%           4                 0   100.00%          42                 5    88.10%           6                 3    50.00%
src/opensearch/opensearchengine.cpp                                                   177                13    92.66%          50                 0   100.00%         381                 9    97.64%         106                27    74.53%
src/opensearch/opensearchengineaction.cpp                                               9                 0   100.00%           2                 0   100.00%          16                 0   100.00%           4                 0   100.00%
src/opensearch/opensearchenginedelegate.cpp                                             2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/opensearch/opensearchenginemodel.cpp                                              111                22    80.18%           9                 0   100.00%         142                27    80.99%         100                31    69.00%
src/opensearch/opensearchmanager.cpp                                                  387                62    83.98%          60                 4    93.33%         697                91    86.94%         268                70    73.88%
src/opensearch/opensearchreader.cpp                                                    20                 1    95.00%           3                 0   100.00%          38                 0   100.00%          12                 2    83.33%
src/opensearch/opensearchwriter.cpp                                                   113                 2    98.23%           3                 0   100.00%          83                 2    97.59%          30                 5    83.33%
src/pictureinpicture.cpp                                                              159                60    62.26%          33                 7    78.79%         332               121    63.55%         102                59    42.16%
src/pipwindow.cpp                                                                      42                14    66.67%          14                 7    50.00%         124                25    79.84%          18                12    33.33%
src/pipwindow.h                                                                         5                 2    60.00%           5                 2    60.00%           5                 2    60.00%           0                 0         -
src/plaintexteditsearch.cpp                                                            17                 2    88.24%           4                 1    75.00%          34                 4    88.24%          10                 4    60.00%
src/popupblocker.cpp                                                                   87                 2    97.70%          15                 0   100.00%         129                 2    98.45%          68                 8    88.24%
src/popupblocker.h                                                                      1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/qrcodedialog.cpp                                                                   44                 9    79.55%           4                 0   100.00%          85                 4    95.29%          30                11    63.33%
src/qrcodegen.cpp                                                                     474                43    90.93%          50                 6    88.00%         571                54    90.54%         342                49    85.67%
src/readermode.cpp                                                                    293               117    60.07%          39                15    61.54%         647               230    64.45%         162                88    45.68%
src/readermode.h                                                                        7                 0   100.00%           3                 0   100.00%           6                 0   100.00%           4                 1    75.00%
src/rustcore/include/rustcore.h                                                         0                 0         -           0                 0         -           0                 0         -           0                 0         -
src/rustcorebridge.cpp                                                                  9                 1    88.89%           5                 0   100.00%          24                 0   100.00%           2                 1    50.00%
src/rustdownloadengine.cpp                                                            292                73    75.00%          17                 0   100.00%         391                80    79.54%         228                83    63.60%
src/rustdownloadengine.h                                                               11                 2    81.82%          11                 2    81.82%          11                 2    81.82%           0                 0         -
src/scriptblockinfobar.cpp                                                              3                 0   100.00%           2                 0   100.00%          39                 0   100.00%           0                 0         -
src/scriptcontrolmanager.cpp                                                          155                 9    94.19%          17                 0   100.00%         221                 7    96.83%         112                25    77.68%
src/scriptcontrolmanager.h                                                              2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/searchbar.cpp                                                                      26                 3    88.46%           9                 0   100.00%          77                 1    98.70%          10                 4    60.00%
src/searchbutton.cpp                                                                   35                 2    94.29%           8                 1    87.50%          86                 4    95.35%          20                 3    85.00%
src/searchlineedit.cpp                                                                  5                 0   100.00%           4                 0   100.00%          24                 0   100.00%           0                 0         -
src/securestore.cpp                                                                   271               115    57.56%          29                 1    96.55%         395               103    73.92%         156                81    48.08%
src/settings.cpp                                                                      799               309    61.33%          83                20    75.90%        1904               470    75.32%         658               298    54.71%
src/sidebarpanel.cpp                                                                   87                62    28.74%          20                14    30.00%         243                98    59.67%          54                50     7.41%
src/sitedecisionstore.cpp                                                              45                 4    91.11%          12                 0   100.00%         103                 4    96.12%          26                 8    69.23%
src/sourcehighlighter.cpp                                                              55                13    76.36%           4                 1    75.00%         135                16    88.15%          44                14    68.18%
src/sourceviewer.cpp                                                                   33                 8    75.76%           6                 1    83.33%         120                13    89.17%          14                 9    35.71%
src/startupprofile.h                                                                   26                11    57.69%           9                 1    88.89%          38                 9    76.32%           8                 4    50.00%
src/statusbarwidgets.cpp                                                               61                 8    86.89%          15                 1    93.33%         166                10    93.98%          28                11    60.71%
src/statusbarwidgets.h                                                                  2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/streamingutils.h                                                                   18                 1    94.44%           1                 0   100.00%          22                 0   100.00%          12                 2    83.33%
src/tabbar.cpp                                                                        469                75    84.01%          64                 3    95.31%         934                93    90.04%         370               124    66.49%
src/tabpreview.cpp                                                                     56                14    75.00%           7                 0   100.00%         120                19    84.17%          42                17    59.52%
src/tabwidget.cpp                                                                    1196               168    85.95%         126                 8    93.65%        1849               201    89.13%         932               284    69.53%
src/tabwidget.h                                                                         1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/toolbarsearch.cpp                                                                 217                56    74.19%          30                 6    80.00%         429                80    81.35%         170                68    60.00%
src/tor/torcontrol.cpp                                                                 89                16    82.02%          17                 3    82.35%         169                33    80.47%          56                21    62.50%
src/tor/tormanager.cpp                                                                192                35    81.77%          32                 4    87.50%         400                56    86.00%         130                50    61.54%
src/tor/torrequestinterceptor.cpp                                                      45                42     6.67%           2                 1    50.00%          57                53     7.02%          34                34     0.00%
src/tor/torsocks5.cpp                                                                  67                20    70.15%          10                 2    80.00%         130                34    73.85%          42                20    52.38%
src/urlcleaner.cpp                                                                     31                 0   100.00%           4                 0   100.00%          59                 0   100.00%          26                 1    96.15%
src/useragent/useragentmenu.cpp                                                        29                 4    86.21%           6                 0   100.00%          93                 5    94.62%          20                 6    70.00%
src/utils/aroraicon.cpp                                                                61                 6    90.16%          12                 1    91.67%          93                 6    93.55%          38                 8    78.95%
src/utils/edittableview.cpp                                                            28                 3    89.29%           4                 0   100.00%          34                 4    88.24%          18                 6    66.67%
src/utils/edittreeview.cpp                                                             23                 0   100.00%           4                 0   100.00%          26                 0   100.00%          14                 2    85.71%
src/utils/languagemanager.cpp                                                         102                27    73.53%          12                 2    83.33%         183                59    67.76%          62                20    67.74%
src/utils/lineedit.cpp                                                                 69                16    76.81%          17                 1    94.12%         152                19    87.50%          36                 8    77.78%
src/utils/safetext.cpp                                                                  6                 0   100.00%           2                 0   100.00%           9                 0   100.00%           2                 0   100.00%
src/utils/safetext.h                                                                    2                 0   100.00%           2                 0   100.00%          12                 0   100.00%           0                 0         -
src/utils/scopeshortcuts.cpp                                                           48                 6    87.50%           9                 0   100.00%          79                 6    92.41%          40                 8    80.00%
src/utils/singleapplication.cpp                                                        70                37    47.14%           9                 1    88.89%         111                30    72.97%          34                20    41.18%
src/utils/squeezelabel.cpp                                                              7                 0   100.00%           2                 0   100.00%          14                 0   100.00%           4                 0   100.00%
src/utils/treesortfilterproxymodel.cpp                                                  6                 0   100.00%           2                 0   100.00%          10                 0   100.00%           2                 0   100.00%
src/webactionmapper.cpp                                                                39                 1    97.44%           8                 0   100.00%          61                 1    98.36%          22                 3    86.36%
src/webpage.cpp                                                                       659                87    86.80%          69                 5    92.75%        1172               107    90.87%         422               123    70.85%
src/webpage.h                                                                           4                 0   100.00%           4                 0   100.00%           4                 0   100.00%           0                 0         -
src/webpermissionmanager.cpp                                                          126                15    88.10%          17                 0   100.00%         259                21    91.89%         122                20    83.61%
src/webview.cpp                                                                       360               155    56.94%          79                27    65.82%         819               284    65.32%         230               128    44.35%
src/webview.h                                                                           5                 0   100.00%           5                 0   100.00%           5                 0   100.00%           0                 0         -
src/webviewsearch.cpp                                                                  29                 3    89.66%           8                 0   100.00%          74                 3    95.95%          12                 3    75.00%
TOTAL regions 76.38% (18021/23594)  functions 86.79% (2431/2801)  lines 82.57% (38840/47037)
```

<!-- rustcov:start -->

## Rust crates (COV05)

Generated: 2026-10-10 22:17 UTC on commit 71c8565
Toolchain: cargo-llvm-cov 0.9.1

Per-crate total LINE coverage from `cargo llvm-cov --workspace --json --summary-only`, gated at 85% minimum per crate by `make check` (.devin/check-rustcoverage.sh; `ARORA_RUSTCOV_MIN` overrides).

| Crate | Lines |
|-------|-------|
| src/adblock/rust | 90.50% |
| src/rustcore | 91.57% |
| src/rustdl | 89.31% |

<!-- rustcov:end -->
