/*
 * Copyright 2026 The Arora Authors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include <QtTest/QtTest>

#include <qbuffer.h>

#include "bookmarknode.h"
#include "converter.h"
#include "htmlbookmarkswriter.h"
#include "xbelreader.h"

class tst_HtmlBookmarks : public QObject
{
    Q_OBJECT

private slots:
    void writeFailures();
    void outputShape();
    void roundTrip();
    void roundTripNonAscii();
};

static BookmarkNode *sampleTree()
{
    BookmarkNode *root = new BookmarkNode(BookmarkNode::Root);

    BookmarkNode *bar = new BookmarkNode(BookmarkNode::Folder, root);
    bar->title = QLatin1String("Bookmarks Bar");
    bar->expanded = true;

    BookmarkNode *link = new BookmarkNode(BookmarkNode::Bookmark, bar);
    link->title = QLatin1String("Arora");
    link->url = QLatin1String("https://arora.example.com/?a=b&c=d");

    new BookmarkNode(BookmarkNode::Separator, bar);

    BookmarkNode *sub = new BookmarkNode(BookmarkNode::Folder, bar);
    sub->title = QLatin1String("News");
    BookmarkNode *subLink = new BookmarkNode(BookmarkNode::Bookmark, sub);
    subLink->title = QLatin1String("Headlines");
    subLink->url = QLatin1String("http://news.example.com/");

    BookmarkNode *menu = new BookmarkNode(BookmarkNode::Folder, root);
    menu->title = QLatin1String("Bookmarks Menu");
    BookmarkNode *menuLink = new BookmarkNode(BookmarkNode::Bookmark, menu);
    menuLink->title = QLatin1String("Docs <&> \"here\"");
    menuLink->url = QLatin1String("http://docs.example.com/");
    menuLink->desc = QLatin1String("Read the <manual> & more");

    return root;
}

void tst_HtmlBookmarks::writeFailures()
{
    HtmlBookmarksWriter writer;
    QVERIFY(!writer.write(QString(), nullptr));
    QVERIFY(!writer.write(QStringLiteral("/nonexistent-dir/x.html"), nullptr));

    QBuffer closed; // never opened -> not writable
    BookmarkNode *root = new BookmarkNode(BookmarkNode::Root);
    QVERIFY(!writer.write(&closed, root));
    delete root;
}

void tst_HtmlBookmarks::outputShape()
{
    BookmarkNode *root = sampleTree();
    BookmarkNode *bar = root->children().at(0);

    QBuffer buffer;
    QVERIFY(buffer.open(QIODevice::ReadWrite));
    HtmlBookmarksWriter writer;
    QVERIFY(writer.write(&buffer, root, bar));

    const QString html = QString::fromUtf8(buffer.data());
    QVERIFY(html.startsWith(QLatin1String("<!DOCTYPE NETSCAPE-Bookmark-file-1>")));
    QVERIFY(html.contains(QLatin1String("text/html; charset=UTF-8")));
    QVERIFY(html.contains(QLatin1String("<DL><p>")));
    // Only the toolbar folder gets the marker; News stays a plain folder.
    QVERIFY(!html.contains(QLatin1String("PERSONAL_TOOLBAR_FOLDER=\"true\">News</H3>")));
    QVERIFY(html.contains(QLatin1String("PERSONAL_TOOLBAR_FOLDER=\"true\">Bookmarks Bar</H3>")));
    QVERIFY(html.contains(QLatin1String("<DT><A HREF=\"https://arora.example.com/?a=b&amp;c=d\">Arora</A>")));
    QVERIFY(html.contains(QLatin1String("<HR>")));
    QVERIFY(html.contains(QLatin1String("<DT><H3 FOLDED>News</H3>")));
    QVERIFY(html.contains(QLatin1String("<DT><A HREF=\"http://docs.example.com/\">Docs &lt;&amp;&gt; &quot;here&quot;</A>")));
    QVERIFY(html.contains(QLatin1String("<DD>Read the &lt;manual&gt; &amp; more")));

    delete root;
}

// Export -> convertHtmlToXbel -> XbelReader must preserve structure,
// titles and urls.  The Netscape outer <DL> reimports as one extra
// wrapper folder with an empty title, same as a Firefox export does.
void tst_HtmlBookmarks::roundTrip()
{
    BookmarkNode *root = sampleTree();

    QBuffer html;
    QVERIFY(html.open(QIODevice::ReadWrite));
    HtmlBookmarksWriter writer;
    QVERIFY(writer.write(&html, root, root->children().at(0)));

    QBuffer xbel;
    QVERIFY(xbel.open(QIODevice::ReadWrite));
    QCOMPARE(convertHtmlToXbel(QString::fromUtf8(html.data()), &xbel), 0);

    xbel.seek(0);
    XbelReader reader;
    BookmarkNode *imported = reader.read(&xbel);
    QCOMPARE(reader.error(), QXmlStreamReader::NoError);

    QCOMPARE(imported->children().count(), 1);
    BookmarkNode *wrapper = imported->children().at(0);
    QCOMPARE(wrapper->type(), BookmarkNode::Folder);
    QCOMPARE(wrapper->children().count(), 2);

    BookmarkNode *bar = wrapper->children().at(0);
    QCOMPARE(bar->type(), BookmarkNode::Folder);
    QCOMPARE(bar->title, QLatin1String("Bookmarks Bar"));
    QCOMPARE(bar->children().count(), 3);
    QCOMPARE(bar->children().at(0)->type(), BookmarkNode::Bookmark);
    QCOMPARE(bar->children().at(0)->title, QLatin1String("Arora"));
    QCOMPARE(bar->children().at(0)->url, QLatin1String("https://arora.example.com/?a=b&c=d"));
    QCOMPARE(bar->children().at(1)->type(), BookmarkNode::Separator);
    BookmarkNode *sub = bar->children().at(2);
    QCOMPARE(sub->type(), BookmarkNode::Folder);
    QCOMPARE(sub->title, QLatin1String("News"));
    QCOMPARE(sub->children().count(), 1);
    QCOMPARE(sub->children().at(0)->title, QLatin1String("Headlines"));

    BookmarkNode *menu = wrapper->children().at(1);
    QCOMPARE(menu->type(), BookmarkNode::Folder);
    QCOMPARE(menu->title, QLatin1String("Bookmarks Menu"));
    QCOMPARE(menu->children().count(), 1);
    QCOMPARE(menu->children().at(0)->title, QLatin1String("Docs <&> \"here\""));
    QCOMPARE(menu->children().at(0)->url, QLatin1String("http://docs.example.com/"));

    delete root;
    delete imported;
}

void tst_HtmlBookmarks::roundTripNonAscii()
{
    BookmarkNode *root = new BookmarkNode(BookmarkNode::Root);
    BookmarkNode *folder = new BookmarkNode(BookmarkNode::Folder, root);
    folder->title = QString::fromUtf8("Könyvjelzők őű");
    BookmarkNode *link = new BookmarkNode(BookmarkNode::Bookmark, folder);
    link->title = QString::fromUtf8("Árvíztűrő tükörfúrógép");
    link->url = QLatin1String("http://pelda.example/");

    QBuffer html;
    QVERIFY(html.open(QIODevice::ReadWrite));
    HtmlBookmarksWriter writer;
    QVERIFY(writer.write(&html, root));
    QVERIFY(QString::fromUtf8(html.data()).contains(link->title));

    QBuffer xbel;
    QVERIFY(xbel.open(QIODevice::ReadWrite));
    QCOMPARE(convertHtmlToXbel(QString::fromUtf8(html.data()), &xbel), 0);

    xbel.seek(0);
    XbelReader reader;
    BookmarkNode *imported = reader.read(&xbel);
    QCOMPARE(reader.error(), QXmlStreamReader::NoError);
    QCOMPARE(imported->children().count(), 1);
    BookmarkNode *wrapper = imported->children().at(0);
    QCOMPARE(wrapper->children().count(), 1);
    QCOMPARE(wrapper->children().at(0)->title, folder->title);
    QCOMPARE(wrapper->children().at(0)->children().count(), 1);
    QCOMPARE(wrapper->children().at(0)->children().at(0)->title, link->title);
    QCOMPARE(wrapper->children().at(0)->children().at(0)->url, link->url);

    delete root;
    delete imported;
}

QTEST_MAIN(tst_HtmlBookmarks)
#include "tst_htmlbookmarks.moc"
