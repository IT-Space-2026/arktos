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

#include "lineedit.h"
#include "lineedit_p.h"

#include <qapplication.h>
#include <qaction.h>
#include <qevent.h>
#include <qlayout.h>
#include <qmenu.h>
#include <qmimedata.h>
#include <qstyleoption.h>
#include <qpainter.h>
#include <qkeysequence.h>

#include <memory>

#include <qdebug.h>

SideWidget::SideWidget(QWidget *parent)
    : QWidget(parent)
{
}

bool SideWidget::event(QEvent *event)
{
    if (event->type() == QEvent::LayoutRequest)
        emit sizeHintChanged();
    return QWidget::event(event);
}

LineEdit::LineEdit(QWidget *parent)
    : QLineEdit(parent)
    , m_leftLayout(nullptr)
    , m_rightLayout(nullptr)
    , m_pristine(true)
    , m_pendingUserEdit(false)
    , m_deleteBurst(false)
    , m_sanitizesUntrustedInput(false)
{
    init();
}

LineEdit::LineEdit(const QString &contents, QWidget *parent)
    : QLineEdit(contents, parent)
    , m_leftWidget(nullptr)
    , m_rightWidget(nullptr)
    , m_leftLayout(nullptr)
    , m_rightLayout(nullptr)
    , m_lastText(contents)
    , m_pristine(true)
    , m_pendingUserEdit(false)
    , m_deleteBurst(false)
    , m_sanitizesUntrustedInput(false)
{
    init();
}

void LineEdit::init()
{
    m_leftWidget = new SideWidget(this);
    m_leftWidget->resize(0, 0);
    m_leftLayout = new QHBoxLayout(m_leftWidget);
    m_leftLayout->setContentsMargins(0, 0, 0, 0);
    if (isRightToLeft())
        m_leftLayout->setDirection(QBoxLayout::RightToLeft);
    else
        m_leftLayout->setDirection(QBoxLayout::LeftToRight);
    m_leftLayout->setSizeConstraint(QLayout::SetFixedSize);

    m_rightWidget = new SideWidget(this);
    m_rightWidget->resize(0, 0);
    m_rightLayout = new QHBoxLayout(m_rightWidget);
    if (isRightToLeft())
        m_rightLayout->setDirection(QBoxLayout::RightToLeft);
    else
        m_rightLayout->setDirection(QBoxLayout::LeftToRight);
    m_rightLayout->setContentsMargins(0, 0, 0, 0);

    QSpacerItem *horizontalSpacer = new QSpacerItem(0, 0, QSizePolicy::Expanding, QSizePolicy::Minimum);
    m_rightLayout->addItem(horizontalSpacer);

    setWidgetSpacing(3);
    connect(m_leftWidget, &SideWidget::sizeHintChanged,
            this, &LineEdit::updateTextMargins);
    connect(m_rightWidget, &SideWidget::sizeHintChanged,
            this, &LineEdit::updateTextMargins);

    // SRCH10: keep the baseline bookkeeping.  A user edit emits
    // textEdited immediately before its textChanged; a bare
    // textChanged is a programmatic write (setText, clear, url
    // display, completer highlight) and re-baselines the field.
    connect(this, &QLineEdit::textEdited, this, [this](const QString &text) {
        m_pendingUserEdit = true;
        m_pristine = false;
        m_deleteBurst = text.size() < m_lastText.size();
    });
    connect(this, &QLineEdit::textChanged, this, [this](const QString &text) {
        if (m_pendingUserEdit) {
            m_pendingUserEdit = false;
            m_pristine = false;
        } else {
            m_pristine = true;
        }
        m_lastText = text;
    });
}

bool LineEdit::event(QEvent *event)
{
    if (event->type() == QEvent::LayoutDirectionChange) {
        if (isRightToLeft()) {
            m_leftLayout->setDirection(QBoxLayout::RightToLeft);
            m_rightLayout->setDirection(QBoxLayout::RightToLeft);
        } else {
            m_leftLayout->setDirection(QBoxLayout::LeftToRight);
            m_rightLayout->setDirection(QBoxLayout::LeftToRight);
        }
    }
    return QLineEdit::event(event);
}

void LineEdit::addWidget(QWidget *widget, WidgetPosition position)
{
    if (!widget)
        return;

    bool rtl = isRightToLeft();
    if (rtl)
        position = (position == LeftSide) ? RightSide : LeftSide;
    if (position == LeftSide) {
        m_leftLayout->addWidget(widget);
    } else {
        m_rightLayout->insertWidget(1, widget);
    }
}

void LineEdit::removeWidget(QWidget *widget)
{
    if (!widget)
        return;

    m_leftLayout->removeWidget(widget);
    m_rightLayout->removeWidget(widget);
    widget->hide();
}

void LineEdit::setWidgetSpacing(int spacing)
{
    m_leftLayout->setSpacing(spacing);
    m_rightLayout->setSpacing(spacing);
    updateTextMargins();
}

int LineEdit::widgetSpacing() const
{
    return m_leftLayout->spacing();
}

int LineEdit::textMargin(WidgetPosition position) const
{
    int spacing = m_rightLayout->spacing();
    int w = 0;
    if (position == LeftSide)
        w = m_leftWidget->sizeHint().width();
    else
        w = m_rightWidget->sizeHint().width();
    if (w == 0)
        return 0;
    return w + spacing * 2;
}

void LineEdit::updateTextMargins()
{
    int left = textMargin(LineEdit::LeftSide);
    int right = textMargin(LineEdit::RightSide);
    int top = 0;
    int bottom = 0;
    setTextMargins(left, top, right, bottom);
    updateSideWidgetLocations();
}

void LineEdit::updateSideWidgetLocations()
{
    QStyleOptionFrame opt;
    initStyleOption(&opt);
    QRect textRect = style()->subElementRect(QStyle::SE_LineEditContents, &opt, this);
    int spacing = m_rightLayout->spacing();
    textRect.adjust(spacing, 0, -spacing, 0);

    int left = textMargin(LineEdit::LeftSide);

    int midHeight = textRect.center().y() + 1;

    if (m_leftLayout->count() > 0) {
        int leftHeight = midHeight - m_leftWidget->height() / 2;
        int leftWidth = m_leftWidget->width();
        if (leftWidth == 0)
            leftHeight = midHeight - m_leftWidget->sizeHint().height() / 2;
        m_leftWidget->move(textRect.x(), leftHeight);
    }
    textRect.setX(left);
    textRect.setY(midHeight - m_rightWidget->sizeHint().height() / 2);
    textRect.setHeight(m_rightWidget->sizeHint().height());
    m_rightWidget->setGeometry(textRect);
}

void LineEdit::resizeEvent(QResizeEvent *event)
{
    updateSideWidgetLocations();
    QLineEdit::resizeEvent(event);
}

void LineEdit::focusInEvent(QFocusEvent *event)
{
    // SRCH10: whatever the field holds at focus time is the new
    // baseline — and a refocus ends any interrupted delete burst.
    // Exception: Qt::PopupFocusReason only means the completer popup
    // closed and bounced focus back mid-edit — that is not a real
    // refocus, so neither the baseline nor a live delete burst may
    // be reset.
    if (event->reason() != Qt::PopupFocusReason) {
        m_pristine = true;
        m_deleteBurst = false;
    }
    QLineEdit::focusInEvent(event);
}

void LineEdit::keyPressEvent(QKeyEvent *event)
{
    // SEC30: a keyboard paste (Ctrl+V, Shift+Insert, ...) is routed
    // through the sanitizer — QLineEdit has no insertFromMimeData
    // hook, so every gesture that can land clipboard text in the
    // field is intercepted at its entry point.
    if (m_sanitizesUntrustedInput
        && event->matches(QKeySequence::Paste)) {
        pasteSanitized(QClipboard::Clipboard);
        return;
    }
    // SRCH10: one-key clear.  Backspace on baseline text empties the
    // whole field (Chrome/Firefox-style) — without it the key
    // dead-stops, because the location bar homes its cursor to
    // position 0 on navigation where a character-wise Backspace
    // deletes nothing.  The rule only holds until the first real user
    // edit; afterwards Backspace stays character-wise.  Plain
    // Backspace (and Shift, which still maps to it) — Ctrl keeps its
    // delete-word meaning as an ordinary edit.  QLineEdit::clear()
    // deliberately: a virtual dispatch would land in
    // ToolbarSearch::clear(), which also drops recent searches and
    // focus.
    const Qt::KeyboardModifiers mods = event->modifiers();
    if (event->key() == Qt::Key_Backspace
        && (mods == Qt::NoModifier || mods == Qt::ShiftModifier)
        && m_pristine && !text().isEmpty() && !isReadOnly()) {
        QLineEdit::clear();
        return;
    }
    QLineEdit::keyPressEvent(event);
}

void LineEdit::setSanitizesUntrustedInput(bool on)
{
    m_sanitizesUntrustedInput = on;
}

bool LineEdit::sanitizesUntrustedInput() const
{
    return m_sanitizesUntrustedInput;
}

QString LineEdit::sanitizeUntrustedInputText(const QString &text)
{
    // Skip leading whitespace — scam payloads often lead with it.
    int i = 0;
    while (i < text.size() && text.at(i).isSpace())
        ++i;
    // Match the scheme token character by character, tolerating
    // whitespace/control junk inside it ("java\tscript:" is an
    // evasion shape that must still strip).
    static const char kScheme[] = "javascript:";
    int matched = 0;
    int j = i;
    while (j < text.size() && matched < int(sizeof(kScheme) - 1)) {
        const QChar c = text.at(j);
        if (c.toLower() == QLatin1Char(kScheme[matched])) {
            ++matched;
        } else if (!c.isSpace()
                   && c.category() != QChar::Other_Control) {
            break;
        }
        ++j;
    }
    return matched == int(sizeof(kScheme) - 1) ? text.mid(j) : text;
}

void LineEdit::pasteSanitized(QClipboard::Mode mode)
{
    // insert() is the same call the stock paste() lands in — it
    // replaces a live selection, honors readOnly, and emits the
    // user-edit signals so SRCH10 bookkeeping stays right.
    const QString pasted = QApplication::clipboard()->text(mode);
    if (!pasted.isEmpty())
        insert(sanitizeUntrustedInputText(pasted));
}

void LineEdit::contextMenuEvent(QContextMenuEvent *event)
{
    if (!m_sanitizesUntrustedInput) {
        QLineEdit::contextMenuEvent(event);
        return;
    }
    // The stock menu's Paste item pastes directly — build the menu
    // ourselves and swap that item for a sanitized twin.  Qt names
    // its standard actions (the translated label and baked-in
    // shortcut text are not matchable); replacing it outright
    // sidesteps whatever internal receiver Qt wired the original to.
    std::unique_ptr<QMenu> menu(createStandardContextMenu());
    const QKeySequence pasteSequence(QKeySequence::Paste);
    for (QAction *action : menu->actions()) {
        if (action->objectName() == QLatin1String("edit-paste")
            || action->shortcut() == pasteSequence) {
            QAction *sanitized = new QAction(action->icon(),
                                             action->text(), menu.get());
            sanitized->setObjectName(action->objectName());
            sanitized->setShortcut(action->shortcut());
            sanitized->setShortcutVisibleInContextMenu(
                action->isShortcutVisibleInContextMenu());
            sanitized->setEnabled(action->isEnabled());
            menu->insertAction(action, sanitized);
            menu->removeAction(action);
            connect(sanitized, &QAction::triggered, this,
                    [this]() { pasteSanitized(QClipboard::Clipboard); });
        }
    }
    menu->exec(event->globalPos());
}

void LineEdit::mousePressEvent(QMouseEvent *event)
{
    // X11-style middle-click paste carries the PRIMARY selection —
    // the same untrusted-text vector with a different gesture.
    if (m_sanitizesUntrustedInput
        && event->button() == Qt::MiddleButton
        && QApplication::clipboard()->supportsSelection()) {
        setCursorPosition(cursorPositionAt(event->position().toPoint()));
        pasteSanitized(QClipboard::Selection);
        return;
    }
    QLineEdit::mousePressEvent(event);
}

void LineEdit::dropEvent(QDropEvent *event)
{
    if (!m_sanitizesUntrustedInput || !event->mimeData()->hasText()) {
        QLineEdit::dropEvent(event);
        return;
    }
    // Reproduce the stock drop (cursor to drop point, insert text)
    // on the sanitized payload — LocationBar's own dropEvent handles
    // url-shaped drops before ever reaching this one.
    if (isReadOnly()) {
        event->ignore();
        return;
    }
    setCursorPosition(cursorPositionAt(event->position().toPoint()));
    insert(sanitizeUntrustedInputText(event->mimeData()->text()));
    event->acceptProposedAction();
}

QString LineEdit::inactiveText() const
{
    return m_inactiveText;
}

void LineEdit::setInactiveText(QString text)
{
    m_inactiveText = std::move(text);
}

void LineEdit::paintEvent(QPaintEvent *event)
{
    QLineEdit::paintEvent(event);
    if (text().isEmpty() && !m_inactiveText.isEmpty() && !hasFocus()) {
        QStyleOptionFrame panel;
        initStyleOption(&panel);
        QRect textRect = style()->subElementRect(QStyle::SE_LineEditContents, &panel, this);
        int horizontalMargin = 2;
        textRect.adjust(horizontalMargin, 0, 0, 0);
        int left = textMargin(LineEdit::LeftSide);
        int right = textMargin(LineEdit::RightSide);
        textRect.adjust(left, 0, -right, 0);
        QPainter painter(this);
        painter.setPen(palette().brush(QPalette::Disabled, QPalette::Text).color());
        painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, m_inactiveText);
    }
}

