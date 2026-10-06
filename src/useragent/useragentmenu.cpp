/**
 * Copyright (c) 2010, William C. Witt
 * Copyright (c) 2010, Benjamin C. Meyer  <ben@meyerhome.net>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. Neither the name of the Arora nor the names of its contributors
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

#include "useragentmenu.h"

#include "browserapplication.h"
#include "safetext.h"
#include "webpage.h"

#include <qactiongroup.h>
#include <qfile.h>
#include <qinputdialog.h>
#include <qsettings.h>
#include <qxmlstream.h>

#include <qdebug.h>

UserAgentMenu::UserAgentMenu(QWidget *parent)
    : QMenu(parent)
{
    connect(this, &UserAgentMenu::aboutToShow, this, &UserAgentMenu::populateMenu);
}

void UserAgentMenu::populateMenu()
{
    disconnect(this, &UserAgentMenu::aboutToShow, this, &UserAgentMenu::populateMenu);

    // Add default action
    QAction *defaultUserAgent = new QAction(this);
    defaultUserAgent->setText(tr("Default"));
    defaultUserAgent->setCheckable(true);
    connect(defaultUserAgent, &QAction::triggered, this, &UserAgentMenu::switchToDefaultUserAgent);
    QSettings settings;
    defaultUserAgent->setChecked(settings.value(QLatin1String("userAgent")).toString().isEmpty());
    addAction(defaultUserAgent);

    // Add default extra user agents.  A copy installed into the package
    // data directory (make install ships one) overrides the bundled
    // resource so distros and admins can adjust the list.
    const QString installedFile =
            BrowserApplication::installedDataDirectory() + QLatin1String("/useragents.xml");
    if (QFile::exists(installedFile))
        addActionsFromFile(installedFile);
    else
        addActionsFromFile(QLatin1String(":/useragents/useragents.xml"));

    // Add other action
    addSeparator();
    QAction *otherUserAgent = new QAction(this);
    otherUserAgent->setCheckable(true);
    otherUserAgent->setText(tr("Other..."));
    connect(otherUserAgent, &QAction::triggered, this, &UserAgentMenu::switchToOtherUserAgent);
    addAction(otherUserAgent);

    bool usingCustomUserAgent = true;
    QActionGroup *actionGroup = new QActionGroup(this);
    const QList<QAction *> menuActions = actions();
    for (QAction *action : menuActions) {
        actionGroup->addAction(action);
        if (action->isChecked()) {
            usingCustomUserAgent = false;
        }
    }
    otherUserAgent->setChecked(usingCustomUserAgent);
}

void UserAgentMenu::addActionsFromFile(const QString &fileName)
{
    QFile file(fileName);
    if (!file.open(QFile::ReadOnly))
        return;

    QString currentUserAgentString = WebPage::userAgent();
    QXmlStreamReader xml(&file);
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement() && xml.name() == QLatin1String("separator")) {
            addSeparator();
            continue;
        }
        if (xml.isStartElement() && xml.name() == QLatin1String("useragent")) {
            QXmlStreamAttributes attributes = xml.attributes();
            QString title = attributes.value(QLatin1String("description")).toString();
            QString userAgent = attributes.value(QLatin1String("useragent")).toString();

            QAction *action = new QAction(this);
            // The xml is an installable data file — treat its strings
            // as untrusted display text.
            action->setText(SafeText::menu(title));
            action->setData(userAgent);
            action->setToolTip(SafeText::escaped(userAgent));
            action->setCheckable(true);
            action->setChecked(userAgent == currentUserAgentString);
            connect(action, &QAction::triggered, this, &UserAgentMenu::changeUserAgent);
            addAction(action);
        }
    }
    if (xml.hasError()) {
        qDebug() << "Error reading custom user agents" << xml.errorString();
         // ... do error handling
    }
}

void UserAgentMenu::changeUserAgent()
{
    if (QAction *action = qobject_cast<QAction*>(sender())) {
        WebPage::setUserAgent(action->data().toString());
    }
}

void UserAgentMenu::switchToDefaultUserAgent()
{
    WebPage::setUserAgent(QString());
}

void UserAgentMenu::switchToOtherUserAgent()
{
    bool ok;
    QString text = QInputDialog::getText(this, tr("Custom user agent"),
                                          tr("User agent:"), QLineEdit::Normal,
                                          WebPage::userAgent(), &ok, Qt::Sheet);
    if (ok) {
        WebPage::setUserAgent(text);
    }
}

