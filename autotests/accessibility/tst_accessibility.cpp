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

// A11Y01: the client-side accessibility contract — every chrome widget
// a screen reader can focus must carry a name, label buddies must
// resolve, and clickable indicators must expose a button role.  The
// AT-SPI bridge itself is a Qt/platform concern (verified separately
// over D-Bus); these tests pin the app side of the interface.

#include <QtTest/QtTest>
#include <QtGui/QtGui>
#include <qaccessible.h>
#include <qdialog.h>
#include <qlabel.h>
#include <qtemporarydir.h>
#include <qwebengineview.h>

#include "ui_passworddialog.h"
#include "ui_proxy.h"

#include "browserapplication.h"
#include "clearbutton.h"
#include "downloadmanager.h"
#include "locationbar.h"
#include "locationbarsiteicon.h"
#include "privacyindicator.h"
#include "searchbar.h"
#include "searchbutton.h"
#include "searchlineedit.h"
#include "settings.h"
#include "tabwidget.h"
#include "webview.h"
#include "webviewsearch.h"
#include "qtest_arora.h"
#include "qtry.h"

class tst_Accessibility : public QObject
{
    Q_OBJECT

private slots:
    void chromeWidgetNames();
    void privacyIndicator();
    void findBarNames();
    void settingsDialogBuddies();
    void credentialDialogBuddies();
    void downloadModelAccessibleText();
    void tabWidgetNames();
    void bridgeStatus();
};

static QString accessibleNameOf(QWidget *widget)
{
    // The returned interface is owned by Qt — never deleted here.
    QAccessibleInterface *iface =
        QAccessible::queryAccessibleInterface(widget);
    return iface ? iface->text(QAccessible::Name) : QString();
}

static QAccessible::Role accessibleRoleOf(QWidget *widget)
{
    QAccessibleInterface *iface =
        QAccessible::queryAccessibleInterface(widget);
    return iface ? iface->role() : QAccessible::NoRole;
}

// Chrome widgets that carry no visible text must still announce a name.
void tst_Accessibility::chromeWidgetNames()
{
    ClearButton clear;
    QCOMPARE(accessibleNameOf(&clear), QStringLiteral("Clear"));

    SearchButton search;
    QCOMPARE(accessibleNameOf(&search), QStringLiteral("Search"));

    SearchLineEdit searchEdit;
    QCOMPARE(accessibleNameOf(&searchEdit), QStringLiteral("Search"));

    LocationBar bar;
    QCOMPARE(accessibleNameOf(&bar), QStringLiteral("Address Bar"));

    LocationBarSiteIcon icon;
    QCOMPARE(accessibleNameOf(&icon), QStringLiteral("Site Icon"));
    QVERIFY(!icon.accessibleDescription().isEmpty());
}

// The private-browsing indicator used to be a plain QLabel — mouse
// only, announced as static text.  It is now a real button: focusable,
// named, keyboard-activatable.
void tst_Accessibility::privacyIndicator()
{
    PrivacyIndicator indicator;
    QCOMPARE(accessibleNameOf(&indicator),
             QStringLiteral("Private Browsing"));
    QCOMPARE(accessibleRoleOf(&indicator), QAccessible::Button);
    QVERIFY(indicator.focusPolicy() != Qt::NoFocus);
    QVERIFY(!indicator.toolTip().isEmpty());

    // Keyboard activation must reach the same leave-private-mode path
    // as the old mousePressEvent did.
    BrowserApplication::setPrivate(true);
    QTest::keyClick(&indicator, Qt::Key_Space);
    QVERIFY(!BrowserApplication::isPrivate());
}

// The find-in-page bar names its one-character nav buttons and the
// search field itself.
void tst_Accessibility::findBarNames()
{
    QWebEngineView view;
    WebViewSearch bar(&view);

    QCOMPARE(accessibleNameOf(bar.findChild<QToolButton*>(QStringLiteral("previousButton"))),
             QStringLiteral("Find previous"));
    QCOMPARE(accessibleNameOf(bar.findChild<QToolButton*>(QStringLiteral("nextButton"))),
             QStringLiteral("Find next"));
    QLineEdit *edit = bar.findChild<QLineEdit*>(QStringLiteral("searchLineEdit"));
    QVERIFY(edit);
    QCOMPARE(accessibleNameOf(edit), QStringLiteral("Find in page"));
}

// Every settings label that fronts an input is its buddy, so assistive
// tools announce "Home Page" when the line edit takes focus.
void tst_Accessibility::settingsDialogBuddies()
{
    SettingsDialog dialog;

    QCOMPARE(dialog.label_7->buddy(),
             static_cast<QWidget*>(dialog.startupBehavior));
    QCOMPARE(dialog.label_3->buddy(),
             static_cast<QWidget*>(dialog.homeLineEdit));
    QCOMPARE(dialog.label_4->buddy(),
             static_cast<QWidget*>(dialog.expireHistory));
    QCOMPARE(dialog.label_5->buddy(),
             static_cast<QWidget*>(dialog.standardFontButton));
    QCOMPARE(dialog.label_6->buddy(),
             static_cast<QWidget*>(dialog.fixedFontButton));
    QCOMPARE(dialog.label_8->buddy(),
             static_cast<QWidget*>(dialog.languageButton));
    QCOMPARE(dialog.label_2->buddy(),
             static_cast<QWidget*>(dialog.acceptCombo));
    QCOMPARE(dialog.label->buddy(),
             static_cast<QWidget*>(dialog.keepUntilCombo));
    QCOMPARE(dialog.cookieSessionLabel->buddy(),
             static_cast<QWidget*>(dialog.cookieSessionCombo));
    QCOMPARE(dialog.linksInNewWindowLabel->buddy(),
             static_cast<QWidget*>(dialog.openTargetBlankLinksIn));
    QCOMPARE(dialog.linkFromApplicationLabel->buddy(),
             static_cast<QWidget*>(dialog.openLinksFromAppsIn));
    QCOMPARE(dialog.label_9->buddy(),
             static_cast<QWidget*>(dialog.proxyType));
    QCOMPARE(dialog.label_10->buddy(),
             static_cast<QWidget*>(dialog.proxyHostName));
    QCOMPARE(dialog.label_11->buddy(),
             static_cast<QWidget*>(dialog.proxyPort));
    QCOMPARE(dialog.label_12->buddy(),
             static_cast<QWidget*>(dialog.proxyUserName));
    QCOMPARE(dialog.label_13->buddy(),
             static_cast<QWidget*>(dialog.proxyPassword));
    QCOMPARE(dialog.label_14->buddy(),
             static_cast<QWidget*>(dialog.userStyleSheet));
    QCOMPARE(dialog.label_15->buddy(),
             static_cast<QWidget*>(dialog.networkCacheMaximumSizeSpinBox));
    QCOMPARE(dialog.label_16->buddy(),
             static_cast<QWidget*>(dialog.autoFillPasswordFormsCheckBox));
    QCOMPARE(dialog.defaultEngineLabel->buddy(),
             static_cast<QWidget*>(dialog.defaultEngineCombo));

    // Label-less inputs carry their own names.
    QVERIFY(!dialog.defaultEngineCombo->accessibleName().isEmpty());
    QVERIFY(!dialog.downloadsLocation->accessibleName().isEmpty());
    QVERIFY(!dialog.externalDownloadPath->accessibleName().isEmpty());
    QVERIFY(!dialog.minimumFontSizeSpinBox->accessibleName().isEmpty());
    QVERIFY(!dialog.permissionsTree->accessibleName().isEmpty());
    QVERIFY(!dialog.extensionsTree->accessibleName().isEmpty());
    QVERIFY(!dialog.userScriptsList->accessibleName().isEmpty());
}

// The auth prompts now give each field a mnemonic label plus a buddy.
void tst_Accessibility::credentialDialogBuddies()
{
    {
        QDialog dialog;
        Ui_PasswordDialog ui;
        ui.setupUi(&dialog);
        QCOMPARE(ui.label->buddy(),
                 static_cast<QWidget*>(ui.userNameLineEdit));
        QCOMPARE(ui.lblPassword->buddy(),
                 static_cast<QWidget*>(ui.passwordLineEdit));
    }
    {
        QDialog dialog;
        Ui_ProxyDialog ui;
        ui.setupUi(&dialog);
        QCOMPARE(ui.usernameLabel->buddy(),
                 static_cast<QWidget*>(ui.userNameLineEdit));
        QCOMPARE(ui.passwordLabel->buddy(),
                 static_cast<QWidget*>(ui.passwordLineEdit));
    }
}

class SubDownloadManager : public DownloadManager
{
public:
    SubDownloadManager(QWidget *parent = nullptr)
        : DownloadManager(parent)
        {}
};

// Rows are painted by an index widget, so the model must answer for
// the assistive roles itself.
void tst_Accessibility::downloadModelAccessibleText()
{
    QTemporaryDir downloadDir;
    QVERIFY(downloadDir.isValid());
    {
        SubDownloadManager manager;
        manager.setDownloadDirectory(downloadDir.path() + QLatin1Char('/'));
        QTableView *view = manager.findChild<QTableView*>();
        QVERIFY(view);
        QCOMPARE(accessibleNameOf(view), QStringLiteral("Downloads"));

        QAbstractItemModel *model = view->model();
        QVERIFY(model);
        QWebEnginePage *page = manager.retryPage(false);
        QVERIFY(page);
        manager.download(page, QUrl(QStringLiteral(
            "data:text/plain;base64,") +
            QString::fromLatin1(QByteArray("hello world").toBase64())));
        QTRY_COMPARE(model->rowCount(), 1);

        const QModelIndex index = model->index(0, 0);
        QVERIFY(index.isValid());
        const QString name =
            model->data(index, Qt::AccessibleTextRole).toString();
        QVERIFY(!name.isEmpty());
        QVERIFY(!model->data(index, Qt::AccessibleDescriptionRole)
                     .toString().isEmpty());

        DownloadItem *item = manager.findChild<DownloadItem*>();
        QVERIFY(item);
        QVERIFY(!item->accessibleName().isEmpty());
    }
}

// The tab strip announces itself and the corner controls inherit real
// names from their actions.
void tst_Accessibility::tabWidgetNames()
{
    TabWidget tabs;
    QTabBar *tabBar = tabs.findChild<QTabBar*>();
    QVERIFY(tabBar);
    // QAccessibleTabBar maps names onto the tab children rather than
    // the bar's own text(Name), so check the widget property itself.
    QCOMPARE(tabBar->accessibleName(), QStringLiteral("Tabs"));

    QList<QToolButton*> corners = tabs.findChildren<QToolButton*>();
    QVERIFY(!corners.isEmpty());
    for (QToolButton *button : corners)
        QVERIFY(!accessibleNameOf(button).isEmpty());
}

// Report whether the platform a11y bridge is live in this environment.
// Under the offscreen QPA there is no bridge, so inactivity is expected;
// run under X11 with an a11y bus to exercise the real AT-SPI path:
//
//   dbus-run-session -- sh -c '
//       /usr/libexec/at-spi-bus-launcher --launch-immediately &
//       xvfb-run -a env QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1
//           ./tst_accessibility bridgeStatus'
//
// (xcb also needs libxcb-cursor0 on some distros — user-local deb
// extraction into LD_LIBRARY_PATH works when it is missing.)
void tst_Accessibility::bridgeStatus()
{
    const bool expectActive =
        qEnvironmentVariableIsSet("QT_LINUX_ACCESSIBILITY_ALWAYS_ON");
    qWarning("QAccessible::isActive() = %d", QAccessible::isActive());

    // The bridge installs lazily once an accessibility client is
    // requested, so show a widget and pump the event loop first.
    QWidget widget;
    widget.setAccessibleName(QStringLiteral("bridge-probe"));
    widget.show();
    QVERIFY(QAccessible::queryAccessibleInterface(&widget));
    QCoreApplication::processEvents();
    QTest::qWait(50);
    qWarning("after event pump: QAccessible::isActive() = %d",
             QAccessible::isActive());

    if (expectActive)
        QVERIFY(QAccessible::isActive());
}

QTEST_MAIN(tst_Accessibility)
#include "tst_accessibility.moc"
