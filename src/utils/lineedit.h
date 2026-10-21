/**
 * Copyright © 2026-2027 Arktos Browser - IT-Space
 * Copyright (c) 2008 - 2009, Arora Browser
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

#ifndef LINEEDIT_H
#define LINEEDIT_H

#include <qlineedit.h>
#include <qclipboard.h>

class QFocusEvent;
class QHBoxLayout;
class QKeyEvent;

/*
    LineEdit is a subclass of QLineEdit that provides an easy and simple
    way to add widgets on the left or right hand side of the text.

    The layout of the widgets on either side are handled by a QHBoxLayout.
    You can set the spacing around the widgets with setWidgetSpacing().

    As widgets are added to the class they are inserted from the outside
    into the center of the widget.
*/
class SideWidget;
class LineEdit : public QLineEdit
{
    Q_OBJECT
    Q_PROPERTY(QString inactiveText READ inactiveText WRITE setInactiveText)

public:
    enum WidgetPosition {
        LeftSide,
        RightSide
    };

    LineEdit(QWidget *parent = nullptr);
    LineEdit(const QString &contents, QWidget *parent = nullptr);

    void addWidget(QWidget *widget, WidgetPosition position);
    void removeWidget(QWidget *widget);
    void setWidgetSpacing(int spacing);
    int widgetSpacing() const;
    int textMargin(WidgetPosition position) const;
    QString inactiveText() const;
    void setInactiveText(QString text);

    void paintEvent(QPaintEvent *event) override;

    // SEC30: self-XSS gate for omnibox-style fields.  When on, text
    // arriving through a clipboard gesture (Ctrl+V, the standard
    // context menu's Paste item, X11 middle-click PRIMARY) or a text
    // drop loses a leading "javascript:" scheme token before it is
    // inserted — matching Chrome's omnibox sanitizer, so a scam's
    // "paste this into your address bar" lands as inert text instead
    // of a payload Enter would run in the current page's origin
    // (WebView::loadUrl's javascript: branch, SEC02).  Typed input is
    // untouched — typed javascript: keeps working, as do
    // programmatic writes.  Off by default: plain LineEdits have
    // legitimate javascript: clipboard flows (the bookmark dialog's
    // address field accepts bookmarklet urls).
    void setSanitizesUntrustedInput(bool on);
    bool sanitizesUntrustedInput() const;
    // Strip a leading "javascript:" token from untrusted text —
    // case-insensitive and tolerant of whitespace/control junk inside
    // the token ("  JAVA\tSCRIPT:..." still strips); text that does
    // not carry the full scheme returns unchanged.
    static QString sanitizeUntrustedInputText(const QString &text);

protected:
    void resizeEvent(QResizeEvent *event) override;
    bool event(QEvent *event) override;
    void focusInEvent(QFocusEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void contextMenuEvent(QContextMenuEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void dropEvent(QDropEvent *event) override;

    // SRCH10: true while the user's latest edit shrank the text — a
    // Backspace/Delete burst.  Completion code must not re-extend the
    // field or re-arm suggestions until the next forward edit
    // (Chrome's rule: backspace kills the inline suggestion).
    bool deletingText() const { return m_deleteBurst; }
    // An explicit completion pick commits forward intent.
    void endDeleteBurst() { m_deleteBurst = false; }

protected slots:
    void updateTextMargins();

private:
    void init();
    void updateSideWidgetLocations();
    void pasteSanitized(QClipboard::Mode mode);

    SideWidget *m_leftWidget;
    SideWidget *m_rightWidget;
    QHBoxLayout *m_leftLayout;
    QHBoxLayout *m_rightLayout;
    QString m_inactiveText;

    // SRCH10: one-key clear bookkeeping.  m_pristine holds while the
    // text is still the field's baseline — what it had at focus-in or
    // its last programmatic write (url display, completer highlight,
    // Escape restore); the first real user edit drops it.  A user
    // edit emits textEdited immediately before textChanged, so the
    // pending flag tells the paired emissions apart from a bare
    // programmatic textChanged.
    QString m_lastText;
    bool m_pristine;
    bool m_pendingUserEdit;
    bool m_deleteBurst;
    bool m_sanitizesUntrustedInput;
};

#endif // LINEEDIT_H

