/*
 * Copyright 2008 Benjamin C. Meyer <ben@meyerhome.net>
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

#include <QtCore/QCoreApplication>
#include <QtCore/QFile>
#include <QtCore/QHash>
#include <QtCore/QStack>
#include <QtCore/QUrl>
#include <QtCore/QXmlStreamWriter>
#include <QtGui/QTextDocumentFragment>

#include <tuple>

/*!
    A tool to convert html bookmark files into the xbel format.

    The html bookmark files should be DOCTYPE: NETSCAPE-Bookmark-file-1

    More information about XBel can be found here: http://pyxml.sourceforge.net/topics/xbel/
*/

// The Netscape bookmark format is HTML, not XML: unclosed <DT>/<HR>/<DD>
// elements and valueless attributes (FOLDED) make a strict QXmlStreamReader
// unusable, so the file is scanned with a small forgiving tag tokenizer
// (the QtWebKit DOM + extract.js approach of the Qt4 tool).

struct HtmlTag {
    QString name;                       // lowercased
    QHash<QString, QString> attributes; // lowercased names
    bool closing = false;
};

// Decodes HTML entities and strips any markup inside an element body.
static QString htmlToText(const QString &fragment)
{
    return QTextDocumentFragment::fromHtml(fragment).toPlainText();
}

// Parses a start or end tag starting at html[pos] == '<'.
// On success *pos is left just past '>' and true is returned.
static bool parseTag(const QString &html, int *pos, HtmlTag *tag)
{
    int i = *pos + 1;
    if (i < html.size() && html.at(i) == QLatin1Char('/')) {
        tag->closing = true;
        ++i;
    }
    int nameStart = i;
    while (i < html.size()
           && (html.at(i).isLetterOrNumber() || html.at(i) == QLatin1Char('-')
               || html.at(i) == QLatin1Char(':')))
        ++i;
    tag->name = html.mid(nameStart, i - nameStart).toLower();
    if (tag->name.isEmpty())
        return false;

    while (i < html.size()) {
        while (i < html.size() && html.at(i).isSpace())
            ++i;
        if (i >= html.size() || html.at(i) == QLatin1Char('>')) {
            ++i;
            break;
        }
        if (html.at(i) == QLatin1Char('/')) {
            ++i;
            continue;
        }
        int attrStart = i;
        while (i < html.size() && !html.at(i).isSpace()
               && html.at(i) != QLatin1Char('=') && html.at(i) != QLatin1Char('>')
               && html.at(i) != QLatin1Char('/'))
            ++i;
        QString attrName = html.mid(attrStart, i - attrStart).toLower();
        while (i < html.size() && html.at(i).isSpace())
            ++i;
        QString value;
        if (i < html.size() && html.at(i) == QLatin1Char('=')) {
            ++i;
            while (i < html.size() && html.at(i).isSpace())
                ++i;
            if (i < html.size()
                && (html.at(i) == QLatin1Char('"') || html.at(i) == QLatin1Char('\''))) {
                QChar quote = html.at(i++);
                int valueStart = i;
                while (i < html.size() && html.at(i) != quote)
                    ++i;
                value = html.mid(valueStart, i - valueStart);
                if (i < html.size())
                    ++i;
            } else {
                int valueStart = i;
                while (i < html.size() && !html.at(i).isSpace()
                       && html.at(i) != QLatin1Char('>'))
                    ++i;
                value = html.mid(valueStart, i - valueStart);
            }
        }
        if (!attrName.isEmpty())
            tag->attributes[attrName] = htmlToText(value);
    }
    *pos = i;
    return true;
}

// Returns the offset of the next "</name" sequence terminated by '>',
// whitespace or '/', or -1.  The scan window is bounded — element
// bodies are titles, i.e. short — so a hostile file full of unclosed
// tags cannot turn the scan quadratic.
static int findClosingTag(const QString &html, int pos, const QString &name)
{
    const int end = qMin(html.size(), pos + 256 * 1024);
    const QString needle = QLatin1String("</") + name;
    for (;;) {
        pos = html.indexOf(needle, pos, Qt::CaseInsensitive);
        if (pos < 0 || pos + needle.size() > end)
            return -1;
        int after = pos + needle.size();
        if (after >= html.size() || html.at(after) == QLatin1Char('>')
            || html.at(after).isSpace() || html.at(after) == QLatin1Char('/'))
            return pos;
        ++pos;
    }
}

// Percent-encodes a url the way the original tool's encodeURI() call did.
static QString encodeUrl(const QString &url)
{
    static const QByteArray excluded = ";,/?:@&=+$-_.!~*'()#";
    return QString::fromLatin1(QUrl::toPercentEncoding(url, excluded));
}

int main(int argc, char **argv)
{
    QCoreApplication application(argc, argv);

    QFile inFile;
    QFile outFile;

    // Either read in from stdin and output to stdout
    // or read in from a file and output to a file
    // Example: ./app foo.html -o bar.xbel
    bool setInput = false;
    bool setOutput = false;
    QStringList args = application.arguments();
    args.takeFirst();
    for (const QString &arg : args) {
        if (arg == QLatin1String("-o")) {
            setOutput = true;
        } else if (setOutput) {
            outFile.setFileName(arg);
            if (!outFile.open(QIODevice::WriteOnly)) {
                qWarning() << "Unable to open" << arg << "for writing";
                return 1;
            }
        } else if (QFile::exists(arg)) {
            setInput = true;
            inFile.setFileName(arg);
            if (!inFile.open(QIODevice::ReadOnly)) {
                qWarning() << "Unable to open" << arg << "for reading";
                return 1;
            }
        } else {
            qWarning() << "Usage: htmlToXBel"
                       << "[stdin|htmlfile]" << "[stdout|-o outFile]";
            return 1;
        }
    }

    if (!setInput)
        std::ignore = inFile.open(stdin, QIODevice::ReadOnly);
    if (!setOutput)
        std::ignore = outFile.open(stdout, QIODevice::WriteOnly);
    if (inFile.openMode() == QIODevice::NotOpen
        || outFile.openMode() == QIODevice::NotOpen) {
        qWarning() << "Unable to open streams";
        return 1;
    }

    const QString html = QString::fromUtf8(inFile.readAll());

    QXmlStreamWriter xbel(&outFile);
    xbel.setAutoFormatting(true);
    xbel.writeStartDocument();
    xbel.writeDTD(QLatin1String("<!DOCTYPE xbel>"));
    xbel.writeStartElement(QLatin1String("xbel"));
    xbel.writeAttribute(QLatin1String("version"), QLatin1String("1.0"));

    // Per-<DL> state carried across siblings, mirroring the locals of the
    // old recursive walk(): the H3 seen since the last DL on this level
    // provides the next folder's title and folded flag.
    struct FolderState {
        QString title;
        bool folded = false;
    };
    QStack<FolderState> folders;
    folders.push(FolderState());

    int items = 0;
    int pos = 0;
    while (pos < html.size()) {
        if (html.at(pos) != QLatin1Char('<')) {
            pos = html.indexOf(QLatin1Char('<'), pos);
            if (pos < 0)
                break;
            continue;
        }
        if (html.mid(pos, 4) == QLatin1String("<!--")) {
            int end = html.indexOf(QLatin1String("-->"), pos + 4);
            pos = (end < 0) ? html.size() : end + 3;
            continue;
        }
        if (pos + 1 < html.size()
            && (html.at(pos + 1) == QLatin1Char('!')
                || html.at(pos + 1) == QLatin1Char('?'))) {
            // <!DOCTYPE ...>, <![CDATA[...]>, processing instructions
            int end = html.indexOf(QLatin1Char('>'), pos + 2);
            pos = (end < 0) ? html.size() : end + 1;
            continue;
        }
        HtmlTag tag;
        if (!parseTag(html, &pos, &tag))
            break;

        if (tag.closing) {
            if (tag.name == QLatin1String("dl") && folders.size() > 1) {
                xbel.writeEndElement(); // </folder>
                folders.pop();
            }
            continue;
        }

        if (tag.name == QLatin1String("dl")) {
            const FolderState &state = folders.top();
            xbel.writeStartElement(QLatin1String("folder"));
            xbel.writeAttribute(QLatin1String("folded"),
                                state.folded ? QLatin1String("true") : QLatin1String("false"));
            xbel.writeTextElement(QLatin1String("title"), state.title);
            folders.push(FolderState());
            ++items;
        } else if (tag.name == QLatin1String("hr")) {
            xbel.writeEmptyElement(QLatin1String("separator"));
            ++items;
        } else if (tag.name == QLatin1String("h3")) {
            int end = findClosingTag(html, pos, tag.name);
            folders.top().title = (end < 0) ? QString()
                                            : htmlToText(html.mid(pos, end - pos));
            folders.top().folded = tag.attributes.contains(QLatin1String("folded"));
            if (end >= 0)
                pos = end;
        } else if (tag.name == QLatin1String("a")
                   && tag.attributes.contains(QLatin1String("href"))) {
            int end = findClosingTag(html, pos, tag.name);
            QString title = (end < 0) ? QString()
                                      : htmlToText(html.mid(pos, end - pos));
            xbel.writeStartElement(QLatin1String("bookmark"));
            xbel.writeAttribute(QLatin1String("href"),
                                encodeUrl(tag.attributes.value(QLatin1String("href"))));
            xbel.writeTextElement(QLatin1String("title"), title);
            xbel.writeEndElement();
            ++items;
            if (end >= 0)
                pos = end;
        }
    }

    while (folders.size() > 1) {
        xbel.writeEndElement(); // unclosed <DL>
        folders.pop();
    }
    xbel.writeEndElement(); // </xbel>
    xbel.writeEndDocument();

    if (!items) {
        qWarning() << "Error while extracting bookmarks.";
        return 1;
    }
    return 0;
}
