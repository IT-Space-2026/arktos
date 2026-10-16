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

#include "profilebutton.h"

#include "browserapplication.h"
#include "browsermainwindow.h"
#include "profileidentity.h"
#include "profilemanagerdialog.h"
#include "profilestore.h"

#include <qboxlayout.h>
#include <qevent.h>
#include <qfontmetrics.h>
#include <qinputdialog.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qmenu.h>
#include <qstyle.h>
#include <qstyleoption.h>
#include <qstylepainter.h>
#include <qwidgetaction.h>

NavBarAccentButton::NavBarAccentButton(QWidget *parent)
    : QToolButton(parent)
{
}

QSize NavBarAccentButton::sizeHint() const
{
    // QToolButton::sizeHint() verbatim, except opt.iconSize keeps
    // this button's own size instead of the toolbar's.
    ensurePolished();
    QStyleOptionToolButton opt;
    initStyleOption(&opt);
    opt.iconSize = iconSize();
    int w = 0;
    int h = 0;
    const QFontMetrics fm(fontMetrics());
    if (opt.toolButtonStyle != Qt::ToolButtonTextOnly) {
        w = opt.iconSize.width();
        h = opt.iconSize.height();
    }
    if (opt.toolButtonStyle != Qt::ToolButtonIconOnly) {
        QSize textSize = fm.size(Qt::TextShowMnemonic, text());
        textSize.setWidth(textSize.width()
                          + fm.horizontalAdvance(QLatin1Char(' ')) * 2);
        if (opt.toolButtonStyle == Qt::ToolButtonTextUnderIcon) {
            h += 4 + textSize.height();
            w = qMax(w, textSize.width());
        } else if (opt.toolButtonStyle == Qt::ToolButtonTextBesideIcon) {
            w += 4 + textSize.width();
            h = qMax(h, textSize.height());
        } else { // TextOnly
            w = textSize.width();
            h = textSize.height();
        }
    }
    opt.rect.setSize(QSize(w, h)); // PM_MenuButtonIndicator needs the height
    if (popupMode() == QToolButton::MenuButtonPopup)
        w += style()->pixelMetric(QStyle::PM_MenuButtonIndicator,
                                  &opt, this);
    QSize grown = style()->sizeFromContents(QStyle::CT_ToolButton,
                                            &opt, QSize(w, h), this);
    // The bigger mark fills the face instead of stretching the row:
    // the face caps at a square one pixel over the glyph, and
    // NavBarToolBar's compacted vertical margins hand the extra
    // pixels back so the bar keeps the height the location field
    // drove.  The style only ever clips the pixmap to the label cell
    // after bounding the request to iconSize, so a snug face shrinks
    // the glyph a pixel — never deforms it.  The iconSize floor
    // guards thin styles: the hit-rect never drops below the art it
    // paints.
    const int edge = qMax(QToolButton::sizeHint().height(),
                          iconSize().height()) + 1;
    return grown.expandedTo(iconSize())
        .boundedTo(QSize(edge, edge));
}

void NavBarAccentButton::paintEvent(QPaintEvent *)
{
    // QToolButton::paintEvent() verbatim plus the same iconSize
    // restore — this is where the toolbar clamp would otherwise
    // shrink the glyph back to the shared nav size.
    QStylePainter p(this);
    QStyleOptionToolButton opt;
    initStyleOption(&opt);
    opt.iconSize = iconSize();
    p.drawComplexControl(QStyle::CC_ToolButton, opt);
}

ProfileButton::ProfileButton(QWidget *parent)
    : NavBarAccentButton(parent)
    , m_menu(new QMenu(this))
    , m_headerAvatar(nullptr)
    , m_headerName(nullptr)
    , m_renameAction(nullptr)
{
    setAutoRaise(true);
    setToolButtonStyle(Qt::ToolButtonIconOnly);
    setPopupMode(QToolButton::InstantPopup);
    setMenu(m_menu);

    // Header row — the identity at a glance: circular avatar + the
    // profile name, painted by a QWidgetAction so the row can carry
    // a 32px face taller than a plain menu item.
    QWidget *header = new QWidget(m_menu);
    header->setObjectName(QLatin1String("profileMenuHeader"));
    QHBoxLayout *headerLayout = new QHBoxLayout(header);
    headerLayout->setContentsMargins(10, 8, 10, 8);
    headerLayout->setSpacing(8);
    m_headerAvatar = new QLabel(header);
    m_headerAvatar->setObjectName(QLatin1String("profileMenuAvatar"));
    m_headerAvatar->setFixedSize(32, 32);
    m_headerName = new QLabel(header);
    m_headerName->setObjectName(QLatin1String("profileMenuName"));
    QFont nameFont = m_headerName->font();
    nameFont.setBold(true);
    m_headerName->setFont(nameFont);
    headerLayout->addWidget(m_headerAvatar);
    headerLayout->addWidget(m_headerName, 1);
    QWidgetAction *headerAction = new QWidgetAction(m_menu);
    headerAction->setDefaultWidget(header);
    m_menu->addAction(headerAction);

    m_renameAction = m_menu->addAction(QString());
    m_renameAction->setObjectName(QLatin1String("profileRenameAction"));
    connect(m_renameAction, &QAction::triggered,
            this, &ProfileButton::renameProfile);

    // MPROF05: profile switcher + manager entry in the identity
    // block.  'New Window' lists every registry profile — each row
    // opens a window bound to that persona (MPROF03).  The
    // menu rebuilds on open so manager edits show immediately.
    m_newWindowMenu = new QMenu(m_menu);
    m_newWindowMenu->menuAction()->setObjectName(
        QLatin1String("profileNewWindowMenu"));
    connect(m_newWindowMenu, &QMenu::aboutToShow,
            this, &ProfileButton::populateNewWindowMenu);
    connect(ProfileStore::instance(), &ProfileStore::profilesChanged,
            this, &ProfileButton::populateNewWindowMenu);
    m_menu->addMenu(m_newWindowMenu);
    populateNewWindowMenu();

    m_managerAction = m_menu->addAction(QString());
    m_managerAction->setObjectName(
        QLatin1String("profileManagerAction"));
    connect(m_managerAction, &QAction::triggered, this, [this]() {
        ProfileManagerDialog::showManager(this, personaId());
    });

    m_menu->addSeparator();
    // PROF02: 'Change Picture…' + 'Remove Picture' insert here,
    // directly under the identity block.
    // PROF03: the Clear-* hygiene section and 'Delete Profile'
    // insert below, under their own separator.

    ProfileIdentity *identity = this->identity();
    connect(identity, &ProfileIdentity::nameChanged,
            this, [this](const QString &) { refreshIdentity(); });
    connect(identity, &ProfileIdentity::avatarChanged,
            this, &ProfileButton::refreshIdentity);

    retranslateUi();
    refreshIdentity();
}

QMenu *ProfileButton::profileMenu() const
{
    return m_menu;
}

void ProfileButton::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();
    NavBarAccentButton::changeEvent(event);
}

void ProfileButton::retranslateUi()
{
    // setText feeds screen readers on an icon-only button (MENU13
    // pattern); the visible label lives in the menu header.
    setText(tr("Profile"));
    setToolTip(tr("Profile"));
    setAccessibleName(tr("Profile"));
    m_renameAction->setText(tr("Edit Profile Name..."));
    m_newWindowMenu->setTitle(tr("New Window"));
    m_managerAction->setText(tr("Profile Manager..."));
}

QString ProfileButton::personaId() const
{
    if (BrowserMainWindow *window = BrowserMainWindow::parentWindow(
            const_cast<ProfileButton*>(this)))
        return window->profileId();
    return ProfileStore::activeProfileId();
}

ProfileIdentity *ProfileButton::identity() const
{
    return ProfileIdentity::instanceFor(personaId());
}

void ProfileButton::populateNewWindowMenu()
{
    m_newWindowMenu->clear();
    ProfileStore *store = ProfileStore::instance();
    const QString activeId = personaId();
    for (const ProfileContext &context : store->profiles()) {
        QAction *action = m_newWindowMenu->addAction(
            QIcon(store->avatarPixmap(context, 16)),
            context.name.isEmpty() ? context.id : context.name);
        action->setData(context.id);
        // MPROF03: every registered persona can host a window now —
        // the window binds the row's id and its tabs/stores follow.
        connect(action, &QAction::triggered, this, [context]() {
            if (BrowserApplication *application =
                    BrowserApplication::instance()) {
                BrowserMainWindow *window =
                    application->newMainWindow(context.id);
                window->goHome();
            }
        });
        if (context.id == activeId) {
            // This window's profile — marked so the row reads as
            // "current"; the new window still inherits it.
            action->setCheckable(true);
            action->setChecked(true);
        }
    }
}

void ProfileButton::refreshIdentity()
{
    ProfileIdentity *identity = this->identity();
    // Bake at 2x the face size (min 64) so the icon stays crisp when
    // the caller's setIconSize lands after construction and on
    // fractional scale factors.
    const int extent = qBound(64, iconSize().width() * 2, 128);
    setIcon(QIcon(identity->avatarPixmap(extent)));
    m_headerAvatar->setPixmap(identity->avatarPixmap(32));
    m_headerName->setText(identity->name());
}

void ProfileButton::renameProfile()
{
    ProfileIdentity *identity = this->identity();
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("Profile Name"), tr("Profile name:"),
        QLineEdit::Normal, identity->name(), &ok);
    if (ok)
        identity->setName(name);
}
