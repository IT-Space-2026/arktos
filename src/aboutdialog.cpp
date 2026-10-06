/*
 * Copyright 2007-2008 Benjamin C. Meyer <ben@meyerhome.net>
 * Copyright 2008 Matvey Kozhev <sikon@ubuntu.com>
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

#include "aboutdialog.h"

#include <qdialogbuttonbox.h>
#include <qfile.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qtextedit.h>
#include <qtextstream.h>

AboutDialog::AboutDialog(QWidget *parent)
    : QDialog(parent)
{
    setupUi(this);
    setWindowTitle(tr("About %1").arg(qApp->applicationName()));
    logo->setPixmap(qApp->windowIcon().pixmap(128, 128));
    name->setText(qApp->applicationName());
    version->setText(qApp->applicationVersion());
    // QtWebKit is gone — report the Qt (QtWebEngine/Chromium) version.
    webkitVersion->setText(tr("Qt version: %1").arg(QLatin1String(qVersion())));
    connect(authorsButton, &QPushButton::clicked,
            this, &AboutDialog::authorsButtonClicked);
    connect(licenseButton, &QPushButton::clicked,
            this, &AboutDialog::licenseButtonClicked);
}

void AboutDialog::displayFile(const QString &fileName, const QString &title)
{
    QDialog dialog(this);
    QVBoxLayout layout(&dialog);
    QTextEdit textEdit(&dialog);
    QDialogButtonBox buttonBox(QDialogButtonBox::Close, Qt::Horizontal, &dialog);

    textEdit.setLayoutDirection(Qt::LeftToRight);

    QFile file(fileName);
    if (!file.open(QIODevice::ReadOnly))
        return;

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    QString text = stream.readAll();
    // this is done to force the content of the text editor to be LTR, and monospaced.
    textEdit.setHtml(QString(QLatin1String("<pre>%1</pre>")).arg(text));

    textEdit.setReadOnly(true);
    connect(&buttonBox, &QDialogButtonBox::rejected, &dialog, &QWidget::close);
    buttonBox.setCenterButtons(true);
    layout.addWidget(&textEdit);
    layout.addWidget(&buttonBox);
    layout.setContentsMargins(6, 6, 6, 6);

    dialog.setLayout(&layout);
    dialog.setWindowTitle(title);
    dialog.setWindowFlags(Qt::Sheet);
    dialog.resize(600, 350);
    dialog.exec();
}

void AboutDialog::authorsButtonClicked()
{
    displayFile(QLatin1String(":AUTHORS"), tr("Authors"));
}

void AboutDialog::licenseButtonClicked()
{
    displayFile(QLatin1String(":LICENSE.GPL2"), tr("License"));
}

