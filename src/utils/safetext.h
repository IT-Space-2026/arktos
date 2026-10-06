/**
 * Copyright (c) 2026, Benjamin C. Meyer  <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Benjamin Meyer nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef SAFETEXT_H
#define SAFETEXT_H

#include <qstyleditemdelegate.h>

/*
    Helpers for displaying strings a web page controls (titles, file
    names, suggestion text, engine names...) in browser chrome.  Qt's
    default text format is Qt::AutoText, so any markup-looking content
    would be interpreted as rich text in labels, tooltips, message
    boxes and item delegates — a markup-injection vector.
*/

namespace SafeText
{

/*
    For Qt::AutoText sinks that cannot be switched to Qt::PlainText —
    QToolTip, QMessageBox arguments, item delegates.

    Escaping alone is not enough: Qt::mightBeRichText() does not treat
    "&amp;"/"&quot;" as markup, so an escaped ampersand-only string
    would be rendered plain and display the entity literally.  Wrapping
    the escaped text in a rich-text container guarantees the rich
    renderer runs and decodes the escapes back to the literal
    characters.  Strings with no markup content render identically
    either way.
*/
inline QString escaped(const QString &plain)
{
    return QLatin1String("<qt>") + plain.toHtmlEscaped()
           + QLatin1String("</qt>");
}

/*
    For QAction/menu/tool-button text.  Menus never render HTML, but
    '&' introduces a mnemonic and '\t' splits off a fake shortcut
    column, so a page title could still spoof chrome.
*/
inline QString menu(const QString &plain)
{
    QString text = plain;
    text.replace(QLatin1Char('\t'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('&'), QLatin1String("&&"));
    return text;
}

}

/*
    Item delegate that always paints the display string literally.
    Views fed by web-controlled models (history, bookmarks, downloads,
    autofill, cookie and search-engine dialogs, completer popups)
    install this instead of QStyledItemDelegate.  Only strings that
    would have been rendered as rich text take the escaped path; plain
    strings keep the fast path.
*/
class PlainTextItemDelegate : public QStyledItemDelegate
{
    Q_OBJECT

public:
    explicit PlainTextItemDelegate(QObject *parent = nullptr);

    QString displayText(const QVariant &value, const QLocale &locale) const override;
};

#endif // SAFETEXT_H
