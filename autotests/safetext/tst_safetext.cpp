/*
 * Copyright (c) 2026, Benjamin C. Meyer <ben@meyerhome.net>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include "qtest_arora.h"

#include <downloadmanager.h>
#include <modelmenu.h>
#include <safetext.h>
#include <tabbar.h>
#include <tabwidget.h>
#include <webview.h>

#include <qsignalspy.h>
#include <qstringlistmodel.h>
#include <qtextdocument.h>

/*
    SEC10: page-controlled strings must never render as markup in
    browser chrome.  Qt::AutoText is the default text format, so an
    attacker title like "<img src=x onerror=...>" would be parsed as
    HTML by labels, tooltips and item delegates.
*/
class tst_SafeText : public QObject
{
    Q_OBJECT

private slots:
    void escaped_data();
    void escaped();
    void menu_data();
    void menu();
    void delegateDisplayText_data();
    void delegateDisplayText();
    void downloadItemLabelsArePlain();
    void modelMenuEscapesTitle();
    void tabTitleIsLiteral();
};

// Render an AutoText string exactly like QLabel/QToolTip would.
static QString renderAutoText(const QString &text)
{
    QTextDocument doc;
    if (Qt::mightBeRichText(text))
        doc.setHtml(text);
    else
        doc.setPlainText(text);
    return doc.toPlainText().trimmed();
}

void tst_SafeText::escaped_data()
{
    QTest::addColumn<QString>("plain");
    QTest::newRow("img") << QString::fromLatin1("<img src=x onerror=alert(1)>");
    QTest::newRow("bold") << QString::fromLatin1("<b>bold</b>");
    QTest::newRow("script") << QString::fromLatin1("<script>alert(1)</script>");
    QTest::newRow("ampersand") << QString::fromLatin1("Fish & Chips");
    QTest::newRow("bare-lt") << QString::fromLatin1("a<b");
    QTest::newRow("pre-escaped") << QString::fromLatin1("x &lt; y");
    QTest::newRow("mixed") << QString::fromLatin1("x & y < z > w");
    QTest::newRow("plain") << QString::fromLatin1("just words");
}

void tst_SafeText::escaped()
{
    QFETCH(QString, plain);
    const QString safe = SafeText::escaped(plain);

    // The wrapper must always route the string through the rich
    // renderer so the escapes decode back to the literal characters —
    // "&amp;" alone does not trigger mightBeRichText.
    QVERIFY2(Qt::mightBeRichText(safe), qPrintable(safe));
    QCOMPARE(renderAutoText(safe), plain);
}

void tst_SafeText::menu_data()
{
    QTest::addColumn<QString>("plain");
    QTest::addColumn<QString>("expected");
    QTest::newRow("amp") << QString::fromLatin1("Fish & Chips")
                         << QString::fromLatin1("Fish && Chips");
    QTest::newRow("leading-amp") << QString::fromLatin1("&File")
                                 << QString::fromLatin1("&&File");
    QTest::newRow("tab") << QString::fromLatin1("a\tCtrl+T")
                         << QString::fromLatin1("a Ctrl+T");
    QTest::newRow("newline") << QString::fromLatin1("a\nb\rc")
                             << QString::fromLatin1("a b c");
    QTest::newRow("markup") << QString::fromLatin1("<img src=x>")
                            << QString::fromLatin1("<img src=x>");
    QTest::newRow("plain") << QString::fromLatin1("words")
                           << QString::fromLatin1("words");
}

void tst_SafeText::menu()
{
    QFETCH(QString, plain);
    QFETCH(QString, expected);
    QCOMPARE(SafeText::menu(plain), expected);
}

void tst_SafeText::delegateDisplayText_data()
{
    QTest::addColumn<QString>("plain");
    QTest::newRow("img") << QString::fromLatin1("<img src=x onerror=alert(1)>");
    QTest::newRow("amp") << QString::fromLatin1("Fish & Chips");
    QTest::newRow("pre-escaped") << QString::fromLatin1("x &lt; y");
    QTest::newRow("plain") << QString::fromLatin1("just words");
}

void tst_SafeText::delegateDisplayText()
{
    QFETCH(QString, plain);
    PlainTextItemDelegate delegate;
    const QString shown = delegate.displayText(plain, QLocale());
    QCOMPARE(renderAutoText(shown), plain);
}

void tst_SafeText::downloadItemLabelsArePlain()
{
    // A null request short-circuits init(); the ui is still built.
    DownloadItem item(nullptr, false);
    QCOMPARE(item.fileNameLabel->textFormat(), Qt::PlainText);
    QCOMPARE(item.downloadInfoLabel->textFormat(), Qt::PlainText);
}

void tst_SafeText::modelMenuEscapesTitle()
{
    QStringListModel model(QStringList()
        << QString::fromLatin1("<img src=x>")
        << QString::fromLatin1("Fish & Chips"));
    ModelMenu menu;
    menu.setModel(&model);
    menu.popup(QPoint(0, 0));
    QTRY_VERIFY_WITH_TIMEOUT(!menu.actions().isEmpty(), 5000);
    menu.hide();

    const QString img = QString::fromLatin1("<img src=x>");
    QCOMPARE(menu.actions().at(0)->text(), img);
    QCOMPARE(menu.actions().at(0)->toolTip(), SafeText::escaped(img));
    QCOMPARE(renderAutoText(menu.actions().at(0)->toolTip()), img);

    // '&' must not swallow a character into a mnemonic.
    QCOMPARE(menu.actions().at(1)->text(),
             QString::fromLatin1("Fish && Chips"));
}

void tst_SafeText::tabTitleIsLiteral()
{
    const QString title =
        QString::fromLatin1("<img src=x onerror=alert(1)>&more");
    TabWidget widget;
    widget.newTab();
    WebView *view = widget.currentWebView();
    QSignalSpy titleSpy(view, SIGNAL(titleChanged(QString)));
    widget.loadUrl(QUrl(QString::fromLatin1(
        "data:text/html,%3Ctitle%3E%3Cimg%20src%3Dx%20onerror%3Dalert(1)"
        "%3E%26more%3C/title%3E")),
        TabWidget::CurrentTab);
    // The view first reports "Loading..."; wait for the real title.
    QTRY_COMPARE_WITH_TIMEOUT(view->title(), title, 15000);
    QVERIFY(titleSpy.count() >= 1);

    // Tab label renders plain: literal text, '&' doubled for the
    // mnemonic layer. The label update rides a later signal hop than
    // WebView::title(), so poll rather than compare once.
    QTRY_COMPARE_WITH_TIMEOUT(widget.tabBar()->tabText(0),
                              SafeText::menu(title), 5000);
    // Tooltip renders rich: escaped text decodes back to literal.
    QCOMPARE(widget.tabBar()->tabToolTip(0), SafeText::escaped(title));
    QCOMPARE(renderAutoText(widget.tabBar()->tabToolTip(0)), title);
}

QTEST_MAIN(tst_SafeText)
#include "tst_safetext.moc"
