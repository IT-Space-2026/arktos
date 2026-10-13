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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "readersanitizer.h"

#include <qhash.h>
#include <qset.h>
#include <qstringlist.h>

// The sanitizer is a purpose-built HTML fragment filter — a real
// tokenizer, not a regex pass.  Source text is never copied into the
// output as markup: text runs go out verbatim (they are character
// data by construction, so they cannot carry tags), and elements are
// re-emitted from the parsed name + scrubbed attribute list.  That
// keeps the usual mutation-XSS tricks (nested comments, bogus
// markup, foreign-content context switches) inert: whatever this
// parser believes is structure is the only structure the browser
// can re-parse.
namespace {

bool isSpace(QChar c)
{
    return c == u' ' || c == u'\t' || c == u'\n' || c == u'\f'
        || c == u'\r';
}

bool isLetter(QChar c)
{
    return (c >= u'a' && c <= u'z') || (c >= u'A' && c <= u'Z');
}

bool isDigit(QChar c)
{
    return c >= u'0' && c <= u'9';
}

bool isHexDigit(QChar c)
{
    return isDigit(c) || (c >= u'a' && c <= u'f')
        || (c >= u'A' && c <= u'F');
}

// ---- entities ---------------------------------------------------------

// Named references.  The security checks only ever need references
// that decode to ASCII (no named reference produces an ASCII letter,
// so "javascript:" can only be spelled via &colon; or numeric
// references) — but a modest common set is decoded anyway so the
// text-length accounting tracks the rendered page.  Names consumed
// without a trailing semicolon are restricted to the legacy HTML
// set, matching what documents have always done.
const QHash<QString, QChar> &namedEntities()
{
    static const QHash<QString, QChar> entities = {
        {QStringLiteral("amp"), u'&'},
        {QStringLiteral("lt"), u'<'},
        {QStringLiteral("gt"), u'>'},
        {QStringLiteral("quot"), u'"'},
        {QStringLiteral("apos"), u'\''},
        {QStringLiteral("colon"), u':'},
        {QStringLiteral("semi"), u';'},
        {QStringLiteral("period"), u'.'},
        {QStringLiteral("comma"), u','},
        {QStringLiteral("excl"), u'!'},
        {QStringLiteral("quest"), u'?'},
        {QStringLiteral("num"), u'#'},
        {QStringLiteral("dollar"), u'$'},
        {QStringLiteral("percnt"), u'%'},
        {QStringLiteral("ast"), u'*'},
        {QStringLiteral("plus"), u'+'},
        {QStringLiteral("sol"), u'/'},
        {QStringLiteral("equals"), u'='},
        {QStringLiteral("commat"), u'@'},
        {QStringLiteral("lsqb"), u'['},
        {QStringLiteral("bsol"), u'\\'},
        {QStringLiteral("rsqb"), u']'},
        {QStringLiteral("Hat"), u'^'},
        {QStringLiteral("lowbar"), u'_'},
        {QStringLiteral("grave"), u'`'},
        {QStringLiteral("lcub"), u'{'},
        {QStringLiteral("verbar"), u'|'},
        {QStringLiteral("rcub"), u'}'},
        {QStringLiteral("lpar"), u'('},
        {QStringLiteral("rpar"), u')'},
        {QStringLiteral("Tab"), u'\t'},
        {QStringLiteral("NewLine"), u'\n'},
        {QStringLiteral("nbsp"), QChar(0x00a0)},
        {QStringLiteral("copy"), QChar(0x00a9)},
        {QStringLiteral("reg"), QChar(0x00ae)},
        {QStringLiteral("deg"), QChar(0x00b0)},
        {QStringLiteral("plusmn"), QChar(0x00b1)},
        {QStringLiteral("middot"), QChar(0x00b7)},
        {QStringLiteral("laquo"), QChar(0x00ab)},
        {QStringLiteral("raquo"), QChar(0x00bb)},
        {QStringLiteral("times"), QChar(0x00d7)},
        {QStringLiteral("divide"), QChar(0x00f7)},
        {QStringLiteral("hellip"), QChar(0x2026)},
        {QStringLiteral("mdash"), QChar(0x2014)},
        {QStringLiteral("ndash"), QChar(0x2013)},
        {QStringLiteral("lsquo"), QChar(0x2018)},
        {QStringLiteral("rsquo"), QChar(0x2019)},
        {QStringLiteral("ldquo"), QChar(0x201c)},
        {QStringLiteral("rdquo"), QChar(0x201d)},
    };
    return entities;
}

// The no-semicolon legacy set (a subset of namedEntities()).
bool isLegacyEntity(const QString &name)
{
    static const QSet<QString> legacy = {
        QStringLiteral("amp"), QStringLiteral("lt"),
        QStringLiteral("gt"), QStringLiteral("quot"),
        QStringLiteral("nbsp"), QStringLiteral("copy"),
        QStringLiteral("reg"), QStringLiteral("deg"),
        QStringLiteral("plusmn"), QStringLiteral("middot"),
        QStringLiteral("laquo"), QStringLiteral("raquo"),
        QStringLiteral("times"), QStringLiteral("divide"),
        QStringLiteral("cent"), QStringLiteral("pound"),
        QStringLiteral("yen"), QStringLiteral("sect"),
        QStringLiteral("para"), QStringLiteral("micro"),
        QStringLiteral("iexcl"), QStringLiteral("iquest"),
        QStringLiteral("frac14"), QStringLiteral("frac12"),
        QStringLiteral("frac34"),
    };
    return legacy.contains(name);
}

// Decode entities the way a document would, for use in checks and
// text accounting — never for re-emission (output text/attributes are
// escaped independently).
QString decodeEntities(const QString &text)
{
    const int amp = text.indexOf(u'&');
    if (amp < 0)
        return text;
    QString out = text.left(amp);
    out.reserve(text.size());
    const int n = text.size();
    int i = amp;
    while (i < n) {
        if (text.at(i) != u'&') {
            out += text.at(i);
            ++i;
            continue;
        }
        int j = i + 1;
        if (j < n && text.at(j) == u'#') {
            ++j;
            bool hex = false;
            if (j < n && (text.at(j) == u'x' || text.at(j) == u'X')) {
                hex = true;
                ++j;
            }
            const int start = j;
            while (j < n && (hex ? isHexDigit(text.at(j))
                                 : isDigit(text.at(j))))
                ++j;
            if (j == start) {
                out += u'&';
                ++i;
                continue;
            }
            const int digitsEnd = j;
            if (j < n && text.at(j) == u';')
                ++j;
            bool ok = false;
            const uint cp = text.mid(start, digitsEnd - start)
                                .toUInt(&ok, hex ? 16 : 10);
            if (!ok || cp == 0 || cp > 0x10ffff
                || (cp >= 0xd800 && cp <= 0xdfff))
                out += QChar(0xfffd);
            else {
                const char32_t u4 = cp;
                out += QString::fromUcs4(&u4, 1);
            }
            i = j;
            continue;
        }
        // Named reference — with-semicolon names first, then the
        // longest legacy (no-semicolon) prefix.
        int k = j;
        while (k < n && k - j < 32 && text.at(k).isLetterOrNumber())
            ++k;
        bool matched = false;
        if (k < n && text.at(k) == u';') {
            const QString name = text.mid(j, k - j);
            const auto it = namedEntities().constFind(name);
            if (it != namedEntities().constEnd()) {
                out += it.value();
                i = k + 1;
                matched = true;
            }
        }
        if (!matched) {
            for (int len = k - j; len > 0; --len) {
                const QString name = text.mid(j, len);
                if (isLegacyEntity(name)
                    && namedEntities().contains(name)) {
                    out += namedEntities().value(name);
                    i = j + len;
                    matched = true;
                    break;
                }
            }
        }
        if (!matched) {
            out += u'&';
            ++i;
        }
    }
    return out;
}

// ---- element policy -----------------------------------------------------

// Removed with their contents; the contents are raw character data
// to the HTML tokenizer (skipped to the matching close tag).
bool isRawTextDrop(const QString &name)
{
    static const QSet<QString> tags = {
        QStringLiteral("script"), QStringLiteral("style"),
        QStringLiteral("textarea"), QStringLiteral("title"),
        QStringLiteral("xmp"), QStringLiteral("iframe"),
        QStringLiteral("noembed"), QStringLiteral("noframes"),
    };
    return tags.contains(name);
}

// Removed with their contents; children are ordinary markup, so the
// skip counts same-name nesting.
bool isContainerDrop(const QString &name)
{
    static const QSet<QString> tags = {
        QStringLiteral("object"), QStringLiteral("applet"),
        QStringLiteral("select"), QStringLiteral("button"),
        QStringLiteral("template"), QStringLiteral("frameset"),
    };
    return tags.contains(name);
}

// Void elements that are removed outright — a void element can have
// no contents, so only the tag itself goes.
bool isVoidDrop(const QString &name)
{
    static const QSet<QString> tags = {
        QStringLiteral("link"), QStringLiteral("meta"),
        QStringLiteral("base"), QStringLiteral("input"),
        QStringLiteral("embed"), QStringLiteral("frame"),
        QStringLiteral("param"), QStringLiteral("basefont"),
        QStringLiteral("bgsound"), QStringLiteral("keygen"),
        QStringLiteral("isindex"), QStringLiteral("menuitem"),
        QStringLiteral("image"), // HTML maps <image> to <img> — drop it
    };
    return tags.contains(name);
}

// Kept void elements — emitted, never pushed (they cannot hold
// contents).
bool isVoidKeep(const QString &name)
{
    static const QSet<QString> tags = {
        QStringLiteral("br"), QStringLiteral("hr"),
        QStringLiteral("img"), QStringLiteral("wbr"),
        QStringLiteral("col"), QStringLiteral("source"),
        QStringLiteral("track"), QStringLiteral("area"),
    };
    return tags.contains(name);
}

// Kept container elements.  Broad but explicit: ordinary flow and
// phrasing markup plus the SVG and MathML vocabularies an article can
// legitimately carry.  Elements not in any set (unknown, obsolete,
// or form/noscript wrappers) are transparent — the tag is dropped
// and the children pass through the same filter.
bool isKeptElement(const QString &name)
{
    static const QSet<QString> tags = {
        // flow / phrasing
        QStringLiteral("a"), QStringLiteral("abbr"),
        QStringLiteral("address"), QStringLiteral("article"),
        QStringLiteral("aside"), QStringLiteral("audio"),
        QStringLiteral("b"), QStringLiteral("bdi"),
        QStringLiteral("bdo"), QStringLiteral("blockquote"),
        QStringLiteral("caption"), QStringLiteral("center"),
        QStringLiteral("cite"), QStringLiteral("code"),
        QStringLiteral("colgroup"), QStringLiteral("data"),
        QStringLiteral("dd"), QStringLiteral("del"),
        QStringLiteral("details"), QStringLiteral("dfn"),
        QStringLiteral("div"), QStringLiteral("dl"),
        QStringLiteral("dt"), QStringLiteral("em"),
        QStringLiteral("figcaption"), QStringLiteral("figure"),
        QStringLiteral("font"), QStringLiteral("footer"),
        QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("header"), QStringLiteral("hgroup"),
        QStringLiteral("i"), QStringLiteral("ins"),
        QStringLiteral("kbd"), QStringLiteral("li"),
        QStringLiteral("main"), QStringLiteral("map"),
        QStringLiteral("mark"), QStringLiteral("nav"),
        QStringLiteral("ol"), QStringLiteral("p"),
        QStringLiteral("picture"), QStringLiteral("pre"),
        QStringLiteral("q"), QStringLiteral("rp"),
        QStringLiteral("rt"), QStringLiteral("ruby"),
        QStringLiteral("s"), QStringLiteral("samp"),
        QStringLiteral("section"), QStringLiteral("small"),
        QStringLiteral("span"), QStringLiteral("strike"),
        QStringLiteral("strong"), QStringLiteral("sub"),
        QStringLiteral("summary"), QStringLiteral("sup"),
        QStringLiteral("table"), QStringLiteral("tbody"),
        QStringLiteral("td"), QStringLiteral("tfoot"),
        QStringLiteral("th"), QStringLiteral("thead"),
        QStringLiteral("time"), QStringLiteral("tr"),
        QStringLiteral("tt"), QStringLiteral("u"),
        QStringLiteral("ul"), QStringLiteral("var"),
        QStringLiteral("video"),
        // svg
        QStringLiteral("svg"), QStringLiteral("animate"),
        QStringLiteral("animatemotion"),
        QStringLiteral("animatetransform"), QStringLiteral("circle"),
        QStringLiteral("clippath"), QStringLiteral("defs"),
        QStringLiteral("desc"), QStringLiteral("ellipse"),
        QStringLiteral("feblend"), QStringLiteral("fecolormatrix"),
        QStringLiteral("fecomponenttransfer"),
        QStringLiteral("fecomposite"), QStringLiteral("feconvolvematrix"),
        QStringLiteral("fediffuselighting"),
        QStringLiteral("fedisplacementmap"),
        QStringLiteral("fedistantlight"), QStringLiteral("fedropshadow"),
        QStringLiteral("feflood"), QStringLiteral("fefunca"),
        QStringLiteral("fefuncb"), QStringLiteral("fefuncg"),
        QStringLiteral("fefuncr"), QStringLiteral("fegaussianblur"),
        QStringLiteral("feimage"), QStringLiteral("femerge"),
        QStringLiteral("femergenode"), QStringLiteral("femorphology"),
        QStringLiteral("feoffset"), QStringLiteral("fepointlight"),
        QStringLiteral("fespecularlighting"),
        QStringLiteral("fespotlight"), QStringLiteral("fetile"),
        QStringLiteral("feturbulence"), QStringLiteral("filter"),
        QStringLiteral("g"), QStringLiteral("line"),
        QStringLiteral("lineargradient"), QStringLiteral("marker"),
        QStringLiteral("mask"), QStringLiteral("mpath"),
        QStringLiteral("path"), QStringLiteral("pattern"),
        QStringLiteral("polygon"), QStringLiteral("polyline"),
        QStringLiteral("radialgradient"), QStringLiteral("rect"),
        QStringLiteral("set"), QStringLiteral("stop"),
        QStringLiteral("switch"), QStringLiteral("symbol"),
        QStringLiteral("text"), QStringLiteral("textpath"),
        QStringLiteral("tspan"), QStringLiteral("use"),
        QStringLiteral("view"),
        // mathml
        QStringLiteral("math"), QStringLiteral("mi"),
        QStringLiteral("mo"), QStringLiteral("mn"),
        QStringLiteral("ms"), QStringLiteral("mtext"),
        QStringLiteral("mrow"), QStringLiteral("mfrac"),
        QStringLiteral("msqrt"), QStringLiteral("mroot"),
        QStringLiteral("msub"), QStringLiteral("msup"),
        QStringLiteral("msubsup"), QStringLiteral("munder"),
        QStringLiteral("mover"), QStringLiteral("munderover"),
        QStringLiteral("mtable"), QStringLiteral("mtr"),
        QStringLiteral("mtd"), QStringLiteral("mlabeledtr"),
        QStringLiteral("mspace"), QStringLiteral("mpadded"),
        QStringLiteral("mphantom"), QStringLiteral("menclose"),
        QStringLiteral("merror"), QStringLiteral("mmultiscripts"),
        QStringLiteral("mprescripts"), QStringLiteral("mstyle"),
        QStringLiteral("semantics"), QStringLiteral("annotation"),
        QStringLiteral("maligngroup"), QStringLiteral("malignmark"),
    };
    return tags.contains(name);
}

// Tags whose kept status feeds the media check (reader.js's
// isReadableBody list).
bool isMediaElement(const QString &name)
{
    return name == QLatin1String("img") || name == QLatin1String("video")
        || name == QLatin1String("audio") || name == QLatin1String("svg")
        || name == QLatin1String("table") || name == QLatin1String("figure");
}

// SVG/MathML vocabulary — inside foreign content a "/>" close is
// honored, unlike HTML where it is ignored.
bool isForeignElement(const QString &name)
{
    static const QSet<QString> html = {
        QStringLiteral("a"), QStringLiteral("abbr"),
        QStringLiteral("address"), QStringLiteral("article"),
        QStringLiteral("aside"), QStringLiteral("audio"),
        QStringLiteral("b"), QStringLiteral("bdi"),
        QStringLiteral("bdo"), QStringLiteral("blockquote"),
        QStringLiteral("caption"), QStringLiteral("center"),
        QStringLiteral("cite"), QStringLiteral("code"),
        QStringLiteral("colgroup"), QStringLiteral("data"),
        QStringLiteral("dd"), QStringLiteral("del"),
        QStringLiteral("details"), QStringLiteral("dfn"),
        QStringLiteral("div"), QStringLiteral("dl"),
        QStringLiteral("dt"), QStringLiteral("em"),
        QStringLiteral("figcaption"), QStringLiteral("figure"),
        QStringLiteral("font"), QStringLiteral("footer"),
        QStringLiteral("h1"), QStringLiteral("h2"),
        QStringLiteral("h3"), QStringLiteral("h4"),
        QStringLiteral("h5"), QStringLiteral("h6"),
        QStringLiteral("header"), QStringLiteral("hgroup"),
        QStringLiteral("i"), QStringLiteral("ins"),
        QStringLiteral("kbd"), QStringLiteral("li"),
        QStringLiteral("main"), QStringLiteral("map"),
        QStringLiteral("mark"), QStringLiteral("nav"),
        QStringLiteral("ol"), QStringLiteral("p"),
        QStringLiteral("picture"), QStringLiteral("pre"),
        QStringLiteral("q"), QStringLiteral("rp"),
        QStringLiteral("rt"), QStringLiteral("ruby"),
        QStringLiteral("s"), QStringLiteral("samp"),
        QStringLiteral("section"), QStringLiteral("small"),
        QStringLiteral("span"), QStringLiteral("strike"),
        QStringLiteral("strong"), QStringLiteral("sub"),
        QStringLiteral("summary"), QStringLiteral("sup"),
        QStringLiteral("table"), QStringLiteral("tbody"),
        QStringLiteral("td"), QStringLiteral("tfoot"),
        QStringLiteral("th"), QStringLiteral("thead"),
        QStringLiteral("time"), QStringLiteral("tr"),
        QStringLiteral("tt"), QStringLiteral("u"),
        QStringLiteral("ul"), QStringLiteral("var"),
        QStringLiteral("video"), QStringLiteral("br"),
        QStringLiteral("hr"), QStringLiteral("img"),
        QStringLiteral("wbr"), QStringLiteral("col"),
        QStringLiteral("source"), QStringLiteral("track"),
        QStringLiteral("area"),
    };
    return isKeptElement(name) && !html.contains(name);
}

// ---- attribute policy -----------------------------------------------------

bool validAttributeName(const QString &name)
{
    if (name.isEmpty() || !isLetter(name.at(0)))
        return false;
    for (const QChar c : name) {
        if (!(c.isLetterOrNumber() || c == u'-' || c == u'_'
              || c == u'.' || c == u':'))
            return false;
    }
    return true;
}

// Attributes whose values are URLs.  Navigable URLs (click targets)
// additionally lose data: — a data:text/html link clicked under
// allow-top-navigation would land an attacker page on the tab.
bool isNavigableUrlAttr(const QString &name)
{
    static const QSet<QString> attrs = {
        QStringLiteral("href"), QStringLiteral("xlink:href"),
        QStringLiteral("action"), QStringLiteral("formaction"),
        QStringLiteral("longdesc"), QStringLiteral("usemap"),
    };
    return attrs.contains(name);
}

bool isResourceUrlAttr(const QString &name)
{
    static const QSet<QString> attrs = {
        QStringLiteral("src"), QStringLiteral("poster"),
        QStringLiteral("background"), QStringLiteral("dynsrc"),
        QStringLiteral("lowsrc"),
    };
    return attrs.contains(name);
}

// Decode, squash ASCII whitespace/controls out (browsers drop
// tab/newline from URLs; the superset keeps the check simple) and
// lowercase for a scheme prefix test.
QString normalizedUrl(const QString &value)
{
    const QString decoded = decodeEntities(value);
    QString out;
    out.reserve(decoded.size());
    for (const QChar c : decoded) {
        if (c > u' ' && c != QChar(0x7f))
            out += c.toLower();
    }
    return out;
}

bool keepAttribute(const QString &name, const QString &value)
{
    if (!validAttributeName(name))
        return false;
    if (name.startsWith(QLatin1String("on")))
        return false;
    if (name == QLatin1String("ping") || name == QLatin1String("srcdoc"))
        return false;
    if (isNavigableUrlAttr(name)) {
        const QString url = normalizedUrl(value);
        if (url.startsWith(QLatin1String("javascript:"))
            || url.startsWith(QLatin1String("vbscript:"))
            || url.startsWith(QLatin1String("data:")))
            return false;
    } else if (isResourceUrlAttr(name)) {
        const QString url = normalizedUrl(value);
        if (url.startsWith(QLatin1String("javascript:"))
            || url.startsWith(QLatin1String("vbscript:")))
            return false;
    }
    return true;
}

// Re-escape a kept attribute value for a double-quoted emission.
// '&' is deliberately preserved: the value was entity-checked on its
// decoded form, and re-emitting entities verbatim keeps the decoded
// result identical on reparse.
void appendEscapedAttribute(QString &out, const QString &value)
{
    for (const QChar c : value) {
        if (c == u'"')
            out += QLatin1String("&quot;");
        else if (c == u'<')
            out += QLatin1String("&lt;");
        else if (c == u'>')
            out += QLatin1String("&gt;");
        else
            out += c;
    }
}

} // namespace

ReaderSanitizer::Result ReaderSanitizer::sanitize(const QString &fragment)
{
    Result result;
    QString out;
    out.reserve(fragment.size());
    QStringList open; // emitted elements awaiting an end tag
    const QString in = fragment;
    const int n = in.size();
    int i = 0;

    // Skips the contents of a raw-text element: to the matching
    // "</name" close (name followed by space, '/', or '>'), then past
    // the closing '>'.
    const auto skipRawText = [&](const QString &name) {
        const QString needle = QLatin1String("</") + name;
        while (i < n) {
            int close = in.indexOf(needle, i, Qt::CaseInsensitive);
            if (close < 0) {
                i = n;
                return;
            }
            const int after = close + needle.size();
            if (after >= n) {
                i = n;
                return;
            }
            const QChar next = in.at(after);
            if (next == u'>' || next == u'/' || isSpace(next)) {
                const int gt = in.indexOf(u'>', after);
                i = gt < 0 ? n : gt + 1;
                return;
            }
            i = after; // "</scriptx" is not a close — keep scanning
        }
    };

    // Skips the contents of an ordinary dropped container, counting
    // same-name nesting.
    const auto skipContainer = [&](const QString &name) {
        const QString openNeedle = QLatin1String("<") + name;
        const QString closeNeedle = QLatin1String("</") + name;
        int depth = 1;
        while (i < n && depth > 0) {
            const int lt = in.indexOf(u'<', i);
            if (lt < 0) {
                i = n;
                return;
            }
            const bool isClose = in.indexOf(closeNeedle, lt,
                                            Qt::CaseInsensitive) == lt;
            const bool isOpen = in.indexOf(openNeedle, lt,
                                           Qt::CaseInsensitive) == lt;
            if (!isClose && !isOpen) {
                i = lt + 1;
                continue;
            }
            const int nameEnd = lt + (isClose ? 2 : 1) + name.size();
            if (nameEnd < n && !isSpace(in.at(nameEnd))
                && in.at(nameEnd) != u'>' && in.at(nameEnd) != u'/') {
                i = nameEnd; // "<selectx" — a different tag
                continue;
            }
            const int gt = in.indexOf(u'>', nameEnd);
            i = gt < 0 ? n : gt + 1;
            depth += isClose ? -1 : 1;
        }
    };

    while (i < n) {
        const int lt = in.indexOf(u'<', i);
        const int textEnd = lt < 0 ? n : lt;
        if (textEnd > i) {
            const QString text = in.mid(i, textEnd - i);
            out += text;
            const QString decoded = decodeEntities(text).trimmed();
            result.textLength += decoded.size();
            i = textEnd;
            continue;
        }
        if (lt < 0)
            break;

        // Markup dispatch — everything unrecognized is literal text.
        const int rest = n - lt;
        if (rest >= 4 && in.mid(lt, 4) == QLatin1String("<!--")) {
            // Comments: "<!-->" and "<!--->" are abrupt closes.
            if (rest >= 5 && in.at(lt + 4) == u'>') {
                i = lt + 5;
            } else if (rest >= 6 && in.mid(lt + 4, 2)
                       == QLatin1String("->")) {
                i = lt + 6;
            } else {
                const int end1 = in.indexOf(QLatin1String("-->"), lt + 4);
                const int end2 = in.indexOf(QLatin1String("--!>"), lt + 4);
                int end = -1;
                if (end1 >= 0 && end2 >= 0)
                    end = qMin(end1, end2);
                else
                    end = qMax(end1, end2);
                i = end < 0 ? n : end + (end == end2 ? 4 : 3);
            }
            continue;
        }
        if (rest >= 2 && (in.at(lt + 1) == u'!' || in.at(lt + 1) == u'?')) {
            // Doctype, CDATA (bogus outside foreign content), and
            // processing instructions are all bogus comments to the
            // HTML tokenizer — they end at the first '>'.
            const int gt = in.indexOf(u'>', lt + 2);
            i = gt < 0 ? n : gt + 1;
            continue;
        }
        if (rest >= 2 && in.at(lt + 1) == u'/') {
            // End tag — honored only for elements actually open.
            int j = lt + 2;
            const int nameStart = j;
            while (j < n && (isLetter(in.at(j)) || isDigit(in.at(j))
                             || in.at(j) == u'-'))
                ++j;
            const QString name =
                in.mid(nameStart, j - nameStart).toLower();
            const int gt = in.indexOf(u'>', j);
            i = gt < 0 ? n : gt + 1;
            const int openAt = open.lastIndexOf(name);
            if (openAt >= 0) {
                while (open.size() > openAt) {
                    out += QLatin1String("</") + open.takeLast()
                        + QLatin1Char('>');
                }
            }
            continue;
        }
        if (rest < 2 || !isLetter(in.at(lt + 1))) {
            // A stray '<' is text.
            out += u'<';
            i = lt + 1;
            continue;
        }

        // Start tag.
        int j = lt + 1;
        const int nameStart = j;
        while (j < n && (isLetter(in.at(j)) || isDigit(in.at(j))
                         || in.at(j) == u'-'))
            ++j;
        const QString name = in.mid(nameStart, j - nameStart).toLower();
        bool selfClosing = false;
        bool tagOpen = false; // saw no '>' — treat rest as consumed
        QString attrs;
        while (j < n) {
            while (j < n && isSpace(in.at(j)))
                ++j;
            if (j >= n)
                break;
            if (in.at(j) == u'>') {
                ++j;
                tagOpen = true;
                break;
            }
            if (in.at(j) == u'/') {
                ++j;
                if (j < n && in.at(j) == u'>') {
                    ++j;
                    selfClosing = true;
                    tagOpen = true;
                    break;
                }
                continue;
            }
            // Attribute name.
            const int aStart = j;
            while (j < n && !isSpace(in.at(j)) && in.at(j) != u'='
                   && in.at(j) != u'>' && in.at(j) != u'/')
                ++j;
            const QString aNameRaw = in.mid(aStart, j - aStart);
            const QString aName = aNameRaw.toLower();
            while (j < n && isSpace(in.at(j)))
                ++j;
            QString aValue;
            if (j < n && in.at(j) == u'=') {
                ++j;
                while (j < n && isSpace(in.at(j)))
                    ++j;
                if (j < n && (in.at(j) == u'"' || in.at(j) == u'\'')) {
                    const QChar quote = in.at(j);
                    ++j;
                    const int vStart = j;
                    while (j < n && in.at(j) != quote)
                        ++j;
                    aValue = in.mid(vStart, j - vStart);
                    if (j < n)
                        ++j;
                } else {
                    const int vStart = j;
                    while (j < n && !isSpace(in.at(j))
                           && in.at(j) != u'>')
                        ++j;
                    aValue = in.mid(vStart, j - vStart);
                }
            }
            if (!aName.isEmpty() && keepAttribute(aName, aValue)) {
                // Emitted with the source's case — foreign content
                // (svg viewBox, xlink:href) is case-sensitive.
                attrs += QLatin1Char(' ') + aNameRaw
                    + QLatin1String("=\"");
                appendEscapedAttribute(attrs, aValue);
                attrs += QLatin1Char('"');
            }
        }
        if (!tagOpen) {
            i = n;
            break;
        }
        i = j;

        if (name == QLatin1String("plaintext")) {
            i = n; // swallows the rest of the document — drop it
            break;
        }
        if (isRawTextDrop(name)) {
            skipRawText(name);
            continue;
        }
        if (isContainerDrop(name)) {
            skipContainer(name);
            continue;
        }
        if (isVoidDrop(name))
            continue;
        if (isVoidKeep(name)) {
            out += QLatin1Char('<') + name + attrs + QLatin1Char('>');
            result.hasMedia |= isMediaElement(name);
            continue;
        }
        if (isKeptElement(name)) {
            out += QLatin1Char('<') + name + attrs;
            if (selfClosing) {
                if (isForeignElement(name))
                    out += QLatin1Char('/');
                // For HTML elements the '/' is ignored — the element
                // opens, exactly as the source parsed.
                else
                    open.append(name);
            } else {
                open.append(name);
            }
            out += QLatin1Char('>');
            result.hasMedia |= isMediaElement(name);
            continue;
        }
        // Transparent: the tag is dropped, children pass through.
    }

    while (!open.isEmpty())
        out += QLatin1String("</") + open.takeLast() + QLatin1Char('>');
    result.html = out;
    return result;
}
