# Coverage baseline (COV01)

Generated: 2026-10-12 21:47 UTC on commit 72e1bd5
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
| Lines | 85.50% |
| Regions | 79.11% |
| Functions | 89.21% |

## Per-file

```
src/aboutdialog.cpp                                                                    12                 1    91.67%           4                 0   100.00%          44                 1    97.73%           2                 1    50.00%
src/acceptlanguagedialog.cpp                                                           71                 6    91.55%          15                 2    86.67%         147                20    86.39%          52                14    73.08%
src/adblock/adblockdialog.cpp                                                          92                15    83.70%          23                 5    78.26%         244                28    88.52%          56                14    75.00%
src/adblock/adblockmanager.cpp                                                        221                32    85.52%          33                 1    96.97%         392                59    84.95%         158                41    74.05%
src/adblock/adblocknetwork.cpp                                                        318                16    94.97%          16                 0   100.00%         407                16    96.07%         268                36    86.57%
src/adblock/adblocknetwork.h                                                            2                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/adblock/adblockpage.cpp                                                           276                17    93.84%          12                 0   100.00%         436                19    95.64%         202                29    85.64%
src/adblock/adblockpresets.cpp                                                          2                 0   100.00%           2                 0   100.00%          88                 0   100.00%           0                 0         -
src/adblock/adblockpresetsdialog.cpp                                                   16                 2    87.50%           2                 0   100.00%          70                 2    97.14%          14                 4    71.43%
src/adblock/adblockrequestinterceptor.cpp                                              83                20    75.90%           7                 1    85.71%         106                16    84.91%          64                13    79.69%
src/adblock/adblockresourcehandler.cpp                                                192                 0   100.00%          12                 0   100.00%         220                 0   100.00%          40                 3    92.50%
src/adblock/adblockrule.cpp                                                           461                18    96.10%          25                 1    96.00%         512                17    96.68%         414                41    90.10%
src/adblock/adblockrule.h                                                              14                 0   100.00%          14                 0   100.00%          14                 0   100.00%           0                 0         -
src/adblock/adblockrustengine.cpp                                                      75                17    77.33%           6                 0   100.00%          99                13    86.87%          62                16    74.19%
src/adblock/adblockrustengine.h                                                         1                 1     0.00%           1                 1     0.00%           1                 1     0.00%           0                 0         -
src/adblock/adblockschemeaccesshandler.cpp                                             20                 1    95.00%           7                 0   100.00%          51                 1    98.04%          10                 2    80.00%
src/adblock/adblocksubscription.cpp                                                   238                57    76.05%          30                 1    96.67%         297                35    88.22%         118                19    83.90%
src/addressdialog.cpp                                                                  49                 4    91.84%           7                 0   100.00%         110                 2    98.18%          38                 5    86.84%
src/argon2id.c                                                                        259                 8    96.91%          18                 0   100.00%         379                 9    97.63%         130                16    87.69%
src/autofilldialog.cpp                                                                 76                25    67.11%           8                 0   100.00%         125                16    87.20%          62                27    56.45%
src/autofillmanager.cpp                                                               397               111    72.04%          50                 5    90.00%         659               107    83.76%         266                90    66.17%
src/autofillmanager.h                                                                  22                 3    86.36%           4                 1    75.00%          10                 1    90.00%          22                12    45.45%
src/autosaver.cpp                                                                      47                 9    80.85%           5                 0   100.00%          36                 5    86.11%          18                 7    61.11%
src/bidiclient.cpp                                                                     92                25    72.83%          14                 1    92.86%         147                26    82.31%          46                24    47.83%
src/bidipanel.cpp                                                                     251                 9    96.41%          17                 0   100.00%         420                11    97.38%         106                11    89.62%
src/bookmarks/addbookmarkdialog.cpp                                                    42                 5    88.10%          14                 3    78.57%         100                13    87.00%          20                 3    85.00%
src/bookmarks/bookmarknode.cpp                                                        105                 8    92.38%          13                 1    92.31%         142                 9    93.66%          74                12    83.78%
src/bookmarks/bookmarknode.h                                                            6                 0   100.00%           4                 0   100.00%           4                 0   100.00%           2                 1    50.00%
src/bookmarks/bookmarksdialog.cpp                                                      56                 4    92.86%          12                 0   100.00%         143                12    91.61%          36                12    66.67%
src/bookmarks/bookmarksmanager.cpp                                                    270                88    67.41%          32                 3    90.62%         444               155    65.09%         160                81    49.38%
src/bookmarks/bookmarksmanager.h                                                        3                 1    66.67%           3                 1    66.67%           9                 3    66.67%           0                 0         -
src/bookmarks/bookmarksmenu.cpp                                                        45                18    60.00%           8                 1    87.50%          76                19    75.00%          24                13    45.83%
src/bookmarks/bookmarksmodel.cpp                                                      199                32    83.92%          20                 0   100.00%         268                49    81.72%         160                44    72.50%
src/bookmarks/bookmarksmodel.h                                                          1                 0   100.00%           1                 0   100.00%           3                 0   100.00%           0                 0         -
src/bookmarks/bookmarkstoolbar.cpp                                                     22                 2    90.91%          10                 0   100.00%          88                 0   100.00%           8                 5    37.50%
src/bookmarks/htmlbookmarkswriter.cpp                                                  32                 4    87.50%           5                 0   100.00%          65                 6    90.77%          28                 7    75.00%
src/bookmarks/xbel/xbelreader.cpp                                                     140                21    85.00%          10                 0   100.00%         154                 9    94.16%         102                34    66.67%
src/bookmarks/xbel/xbelwriter.cpp                                                      29                 2    93.10%           4                 0   100.00%          50                 3    94.00%          24                 4    83.33%
src/browserapplication.cpp                                                            366               123    66.39%          61                 7    88.52%         702               172    75.50%         222               100    54.95%
src/browsermainwindow.cpp                                                             652               155    76.23%         124                19    84.68%        1935               232    88.01%         436               169    61.24%
src/browserpaths.h                                                                      5                 1    80.00%           1                 0   100.00%          10                 1    90.00%           4                 1    75.00%
src/browserprofile.cpp                                                                339                64    81.12%          42                 1    97.62%         696                56    91.95%         232                59    74.57%
src/browsertheme.cpp                                                                   85                 4    95.29%          15                 0   100.00%         167                 4    97.60%          60                12    80.00%
src/clearbutton.cpp                                                                    14                 5    64.29%           3                 0   100.00%          40                14    65.00%           6                 4    33.33%
src/clearprivatedata.cpp                                                              189                 7    96.30%           9                 0   100.00%         287                 7    97.56%         162                26    83.95%
src/commandpalette.cpp                                                                310                41    86.77%          35                 5    85.71%         510                62    87.84%         194                54    72.16%
src/containermanager.cpp                                                              249                21    91.57%          37                 0   100.00%         390                19    95.13%         178                34    80.90%
src/devtoolswindow.cpp                                                                 28                 1    96.43%           5                 0   100.00%          52                 1    98.08%          18                 3    83.33%
src/devtoolswindow.h                                                                    2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/downloadgraph.cpp                                                                  22                 2    90.91%           5                 0   100.00%          60                 5    91.67%          14                 3    78.57%
src/downloadgraph.h                                                                     1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/downloadmanager.cpp                                                               807                87    89.22%          79                 2    97.47%        1218               126    89.66%         520               121    76.73%
src/downloadmanager.h                                                                   6                 0   100.00%           6                 0   100.00%           6                 0   100.00%           0                 0         -
src/downloadworker.cpp                                                                117                39    66.67%           7                 0   100.00%         244                64    73.77%         100                52    48.00%
src/energysaver.cpp                                                                    74                 5    93.24%          13                 1    92.31%         116                 4    96.55%          58                12    79.31%
src/engine/engineinterface.h                                                            9                 1    88.89%           5                 1    80.00%           5                 1    80.00%           0                 0         -
src/engine/engineregistry.cpp                                                          50                11    78.00%          12                 1    91.67%          79                 8    89.87%          24                 8    66.67%
src/engine/enginetab.cpp                                                               36                12    66.67%          13                 6    53.85%          87                18    79.31%          16                 9    43.75%
src/engine/enginetab.h                                                                  4                 0   100.00%           4                 0   100.00%           4                 0   100.00%           0                 0         -
src/engine/navigationpolicy.cpp                                                       363                50    86.23%          57                 4    92.98%         368                47    87.23%          68                20    70.59%
src/engine/webenginebackend.cpp                                                       338               118    65.09%         109                22    79.82%         728               196    73.08%         234               129    44.87%
src/extensions/extensionmanager.cpp                                                   543                82    84.90%          55                 3    94.55%         981               140    85.73%         294               114    61.22%
src/extensions/extensionreviewdialog.cpp                                              132                 4    96.97%           7                 0   100.00%         227                 7    96.92%          78                10    87.18%
src/fingerprintprotector.cpp                                                           66                21    68.18%          11                 1    90.91%         106                29    72.64%          54                28    48.15%
src/history/history.cpp                                                               469                73    84.43%          67                 5    92.54%         727                85    88.31%         332               102    69.28%
src/history/history.h                                                                  13                 1    92.31%           5                 0   100.00%          12                 0   100.00%           8                 4    50.00%
src/history/historycompleter.cpp                                                      365                60    83.56%          59                 7    88.14%         590                76    87.12%         292                82    71.92%
src/history/historymanager.cpp                                                        222                67    69.82%          32                 1    96.88%         385                56    85.45%         130                41    68.46%
src/history/historymanager.h                                                           11                 0   100.00%           4                 0   100.00%           7                 0   100.00%           6                 2    66.67%
src/history/historyparser.cpp                                                          34                12    64.71%           2                 0   100.00%          52                10    80.77%          26                15    42.31%
src/locationbar/engineindicator.cpp                                                    29                10    65.52%           6                 2    66.67%          63                24    61.90%          16                 7    56.25%
src/locationbar/engineindicator.h                                                       2                 1    50.00%           2                 1    50.00%           2                 1    50.00%           0                 0         -
src/locationbar/locationbar.cpp                                                       163                 7    95.71%          15                 0   100.00%         276                 7    97.46%         130                21    83.85%
src/locationbar/locationbarsiteicon.cpp                                                21                11    47.62%           5                 1    80.00%          50                20    60.00%          16                13    18.75%
src/locationbar/omniboxsuggestions.cpp                                                 85                11    87.06%           9                 0   100.00%         103                 9    91.26%          66                17    74.24%
src/locationbar/popupblockerbutton.cpp                                                 68                 7    89.71%          10                 1    90.00%         130                 8    93.85%          40                 8    80.00%
src/locationbar/privacyindicator.cpp                                                   23                 4    82.61%           3                 0   100.00%          45                 4    91.11%          18                 6    66.67%
src/locationbar/readerbutton.cpp                                                       52                 0   100.00%           7                 0   100.00%          83                 0   100.00%          40                 4    90.00%
src/locationbar/sitepanel.cpp                                                         332                80    75.90%          35                 5    85.71%         653                99    84.84%         254                81    68.11%
src/locationbar/siteshield.cpp                                                         80                11    86.25%          14                 1    92.86%         169                13    92.31%          50                11    78.00%
src/locationbar/siteshield.h                                                            2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/main.cpp                                                                         8242              2633    68.05%         509               111    78.19%       19778              3570    81.95%        3888              1718    55.81%
src/modelmenu.cpp                                                                     134                47    64.93%          28                 5    82.14%         258                83    67.83%          82                41    50.00%
src/modeltoolbar.cpp                                                                  101                24    76.24%          15                 0   100.00%         178                27    84.83%          64                30    53.12%
src/network/cookiejar/cookiedialog.cpp                                                 15                 1    93.33%           2                 0   100.00%          57                 1    98.25%          12                 1    91.67%
src/network/cookiejar/cookieexceptionsdialog.cpp                                       15                 0   100.00%           7                 0   100.00%          71                 0   100.00%           8                 1    87.50%
src/network/cookiejar/cookieexceptionsmodel.cpp                                        80                 2    97.50%           8                 0   100.00%         134                 1    99.25%          82                13    84.15%
src/network/cookiejar/cookiejar.cpp                                                   264                31    88.26%          40                 2    95.00%         470                44    90.64%         222                66    70.27%
src/network/cookiejar/cookiemodel.cpp                                                  76                 9    88.16%           7                 0   100.00%         119                 9    92.44%          80                11    86.25%
src/network/cookiejar/networkcookiejar/networkcookiejar.cpp                           138                 9    93.48%          16                 0   100.00%         200                10    95.00%         110                19    82.73%
src/network/cookiejar/networkcookiejar/networkcookiejar_p.h                             6                 0   100.00%           3                 0   100.00%          14                 0   100.00%           2                 1    50.00%
src/network/cookiejar/networkcookiejar/trie_p.h                                        73                12    83.56%          13                 1    92.31%         118                 4    96.61%          46                20    56.52%
src/network/domainblocklist.cpp                                                        91                36    60.44%          11                 0   100.00%         123                19    84.55%          50                17    66.00%
src/network/fileaccesshandler.cpp                                                      59                 7    88.14%           8                 0   100.00%         121                18    85.12%          42                11    73.81%
src/network/networkaccessmanager.cpp                                                   79                34    56.96%          10                 2    80.00%         207                82    60.39%          58                31    46.55%
src/network/networkdiskcache.cpp                                                        9                 1    88.89%           4                 0   100.00%          22                 1    95.45%           2                 1    50.00%
src/network/networkproxyfactory.cpp                                                     9                 0   100.00%           4                 0   100.00%          16                 0   100.00%           6                 0   100.00%
src/network/privacyrequestinterceptor.cpp                                             121                13    89.26%          56                 7    87.50%         308                32    89.61%          64                14    78.12%
src/network/schemeaccesshandler.cpp                                                    34                 3    91.18%          12                 0   100.00%          96                 5    94.79%          12                 5    58.33%
src/network/tlsverifier.cpp                                                            67                 4    94.03%          12                 0   100.00%         148                 4    97.30%          30                 7    76.67%
src/network/trancolist.cpp                                                            103                36    65.05%          11                 0   100.00%         128                19    85.16%          56                19    66.07%
src/opensearch/opensearchdialog.cpp                                                    12                 3    75.00%           4                 0   100.00%          42                 5    88.10%           6                 3    50.00%
src/opensearch/opensearchengine.cpp                                                   202                22    89.11%          53                 1    98.11%         425                24    94.35%         126                34    73.02%
src/opensearch/opensearchengineaction.cpp                                               9                 0   100.00%           2                 0   100.00%          16                 0   100.00%           4                 1    75.00%
src/opensearch/opensearchenginedelegate.cpp                                             2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/opensearch/opensearchenginemodel.cpp                                              111                22    80.18%           9                 0   100.00%         142                27    80.99%         100                31    69.00%
src/opensearch/opensearchmanager.cpp                                                  409                62    84.84%          61                 4    93.44%         725                91    87.45%         280                73    73.93%
src/opensearch/opensearchreader.cpp                                                    20                 1    95.00%           3                 0   100.00%          38                 0   100.00%          12                 2    83.33%
src/opensearch/opensearchwriter.cpp                                                   116                 2    98.28%           3                 0   100.00%          85                 2    97.65%          32                 5    84.38%
src/pdfsupport.cpp                                                                    130                12    90.77%          24                 1    95.83%         217                22    89.86%         104                24    76.92%
src/pdfview.cpp                                                                       565               141    75.04%          71                14    80.28%        1142               200    82.49%         350               138    60.57%
src/pdfview.h                                                                          11                 1    90.91%          11                 1    90.91%          11                 1    90.91%           0                 0         -
src/pdfworker.cpp                                                                      64                19    70.31%           3                 1    66.67%         157                44    71.97%          48                22    54.17%
src/pictureinpicture.cpp                                                              139                45    67.63%          28                 3    89.29%         262                61    76.72%          90                50    44.44%
src/pipwindow.cpp                                                                      42                14    66.67%          14                 7    50.00%         124                25    79.84%          18                12    33.33%
src/pipwindow.h                                                                         4                 1    75.00%           4                 1    75.00%           4                 1    75.00%           0                 0         -
src/plaintexteditsearch.cpp                                                            17                 2    88.24%           4                 1    75.00%          34                 4    88.24%          10                 4    60.00%
src/popupblocker.cpp                                                                   87                 2    97.70%          15                 0   100.00%         129                 2    98.45%          68                 8    88.24%
src/popupblocker.h                                                                      1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/positionpicker.cpp                                                                 66                 7    89.39%          14                 0   100.00%         154                13    91.56%          46                12    73.91%
src/positionpicker.h                                                                    2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/qrcodedialog.cpp                                                                   44                 9    79.55%           4                 0   100.00%          85                 4    95.29%          30                11    63.33%
src/qrcodegen.cpp                                                                     474                43    90.93%          50                 6    88.00%         571                54    90.54%         342                49    85.67%
src/readermode.cpp                                                                    305                95    68.85%          38                 9    76.32%         621               154    75.20%         172                80    53.49%
src/readermode.h                                                                        7                 0   100.00%           3                 0   100.00%           6                 0   100.00%           4                 1    75.00%
src/rustcore/include/rustcore.h                                                         0                 0         -           0                 0         -           0                 0         -           0                 0         -
src/rustcorebridge.cpp                                                                  9                 1    88.89%           5                 0   100.00%          24                 0   100.00%           2                 1    50.00%
src/rustdownloadengine.cpp                                                            455               108    76.26%          32                 2    93.75%         653               121    81.47%         340               119    65.00%
src/rustdownloadengine.h                                                               14                 1    92.86%          14                 1    92.86%          14                 1    92.86%           0                 0         -
src/sandbox/bwrapgenerator.cpp                                                        213                12    94.37%          10                 0   100.00%         210                 3    98.57%          54                14    74.07%
src/sandbox/platformgenerators.cpp                                                    147                11    92.52%           7                 0   100.00%         147                 7    95.24%          30                 4    86.67%
src/sandbox/sandboxmanager.cpp                                                        126                23    81.75%           8                 0   100.00%         113                 9    92.04%          54                14    74.07%
src/sandbox/sandboxpolicy.cpp                                                          80                 1    98.75%           3                 0   100.00%          58                 1    98.28%          12                 3    75.00%
src/scriptblockinfobar.cpp                                                              3                 0   100.00%           2                 0   100.00%          39                 0   100.00%           0                 0         -
src/scriptcontrolmanager.cpp                                                          155                 8    94.84%          17                 0   100.00%         221                 6    97.29%         112                24    78.57%
src/scriptcontrolmanager.h                                                              2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/searchbar.cpp                                                                      26                 3    88.46%           9                 0   100.00%          77                 1    98.70%          10                 4    60.00%
src/searchbutton.cpp                                                                   34                 1    97.06%           7                 0   100.00%          83                 1    98.80%          20                 3    85.00%
src/searchlineedit.cpp                                                                  5                 0   100.00%           4                 0   100.00%          24                 0   100.00%           0                 0         -
src/securestore.cpp                                                                   271               107    60.52%          29                 1    96.55%         395                98    75.19%         156                77    50.64%
src/settings.cpp                                                                     1096               226    79.38%         111                11    90.09%        2582               313    87.88%         888               285    67.91%
src/sidebargrip.cpp                                                                    50                 3    94.00%          11                 0   100.00%          80                 1    98.75%          28                12    57.14%
src/sidebarpanel.cpp                                                                  312               106    66.03%          71                22    69.01%         808               184    77.23%         198               110    44.44%
src/sidebarpositionpicker.cpp                                                          18                 0   100.00%           2                 0   100.00%          36                 0   100.00%          10                 0   100.00%
src/sitedecisionstore.cpp                                                              45                 4    91.11%          12                 0   100.00%         103                 4    96.12%          26                 8    69.23%
src/sourcehighlighter.cpp                                                              55                13    76.36%           4                 1    75.00%         135                16    88.15%          44                14    68.18%
src/sourceviewer.cpp                                                                   33                 8    75.76%           6                 1    83.33%         121                13    89.26%          14                 9    35.71%
src/startupprofile.h                                                                   26                11    57.69%           9                 1    88.89%          38                 9    76.32%           8                 4    50.00%
src/statusbarwidgets.cpp                                                              240                35    85.42%          28                 1    96.43%         442                30    93.21%         148                43    70.95%
src/statusbarwidgets.h                                                                  2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/streamingutils.h                                                                   18                 1    94.44%           1                 0   100.00%          22                 0   100.00%          12                 2    83.33%
src/tabbar.cpp                                                                       1030               145    85.92%         101                 4    96.04%        1725               177    89.74%         850               267    68.59%
src/tabpositionpicker.cpp                                                              23                 0   100.00%           2                 0   100.00%          43                 0   100.00%          16                 0   100.00%
src/tabpreview.cpp                                                                     54                14    74.07%           7                 0   100.00%         118                19    83.90%          40                17    57.50%
src/tabwidget.cpp                                                                    1872               233    87.55%         181                10    94.48%        2782               268    90.37%        1486               409    72.48%
src/tabwidget.h                                                                         1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
src/toolbarsearch.cpp                                                                 220                59    73.18%          30                 6    80.00%         432               115    73.38%         172                70    59.30%
src/tor/torcontrol.cpp                                                                 89                16    82.02%          17                 3    82.35%         169                33    80.47%          56                21    62.50%
src/tor/tormanager.cpp                                                                355                41    88.45%          43                 1    97.67%         745                55    92.62%         268                84    68.66%
src/tor/torrequestinterceptor.cpp                                                      20                 0   100.00%           3                 0   100.00%          48                 0   100.00%          24                 4    83.33%
src/tor/torsocks5.cpp                                                                  67                20    70.15%          10                 2    80.00%         130                34    73.85%          42                20    52.38%
src/urlcleaner.cpp                                                                     31                 0   100.00%           4                 0   100.00%          59                 0   100.00%          26                 1    96.15%
src/useragent/useragentmenu.cpp                                                        29                 4    86.21%           6                 0   100.00%          93                 5    94.62%          20                 6    70.00%
src/utils/aroraicon.cpp                                                                61                 6    90.16%          12                 1    91.67%          93                 6    93.55%          38                 8    78.95%
src/utils/crashreporter.cpp                                                            52                11    78.85%           9                 0   100.00%         113                16    85.84%          42                14    66.67%
src/utils/edittableview.cpp                                                            28                 3    89.29%           4                 0   100.00%          34                 4    88.24%          18                 6    66.67%
src/utils/edittreeview.cpp                                                             23                 0   100.00%           4                 0   100.00%          26                 0   100.00%          14                 2    85.71%
src/utils/languagemanager.cpp                                                         113                28    75.22%          12                 2    83.33%         194                59    69.59%          70                20    71.43%
src/utils/lineedit.cpp                                                                 98                21    78.57%          21                 1    95.24%         204                23    88.73%          54                13    75.93%
src/utils/lineedit.h                                                                    2                 0   100.00%           2                 0   100.00%           2                 0   100.00%           0                 0         -
src/utils/safetext.cpp                                                                  6                 0   100.00%           2                 0   100.00%           9                 0   100.00%           2                 0   100.00%
src/utils/safetext.h                                                                    2                 0   100.00%           2                 0   100.00%          12                 0   100.00%           0                 0         -
src/utils/scopeshortcuts.cpp                                                           48                 6    87.50%           9                 0   100.00%          79                 6    92.41%          40                 8    80.00%
src/utils/singleapplication.cpp                                                        70                37    47.14%           9                 1    88.89%         111                30    72.97%          34                20    41.18%
src/utils/squeezelabel.cpp                                                              7                 1    85.71%           2                 0   100.00%          14                 3    78.57%           4                 1    75.00%
src/utils/terminationsignalhandler.cpp                                                 44                17    61.36%           6                 1    83.33%          83                21    74.70%          24                10    58.33%
src/utils/treesortfilterproxymodel.cpp                                                  6                 0   100.00%           2                 0   100.00%          10                 0   100.00%           2                 0   100.00%
src/webactionmapper.cpp                                                                41                 1    97.56%           8                 0   100.00%          61                 1    98.36%          24                 4    83.33%
src/webpage.cpp                                                                       721               121    83.22%          72                 5    93.06%        1271               169    86.70%         480               155    67.71%
src/webpage.h                                                                           5                 0   100.00%           5                 0   100.00%           5                 0   100.00%           0                 0         -
src/webpermissionmanager.cpp                                                          126                15    88.10%          17                 0   100.00%         259                21    91.89%         122                20    83.61%
src/webview.cpp                                                                       442               152    65.61%          91                24    73.63%         935               238    74.55%         292               136    53.42%
src/webview.h                                                                           5                 0   100.00%           5                 0   100.00%           5                 0   100.00%           0                 0         -
src/webviewsearch.cpp                                                                  42                 9    78.57%          14                 4    71.43%         112                30    73.21%          18                 5    72.22%
src/webviewsearch.h                                                                     1                 0   100.00%           1                 0   100.00%           1                 0   100.00%           0                 0         -
TOTAL regions 79.11% (25900/32739)  functions 89.21% (3375/3783)  lines 85.50% (53777/62899)
```

<!-- rustcov:start -->

## Rust crates (COV05)

Generated: 2026-10-12 19:56 UTC on commit 0210e0d
Toolchain: cargo-llvm-cov 0.9.1

Per-crate total LINE coverage from `cargo llvm-cov --workspace --json --summary-only`, gated at 85% minimum per crate by `make check` (.devin/check-rustcoverage.sh; `ARORA_RUST_COVERAGE_MIN` overrides).

| Crate | Lines |
|-------|-------|
| src/adblock/rust | 90.50% |
| src/rustcore | 91.63% |
| src/rustdl | 89.25% |

<!-- rustcov:end -->
