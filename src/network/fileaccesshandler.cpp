/*
 * Copyright 2009 Jonas Gehring <jonas.gehring@boolsoft.org>
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

#include "fileaccesshandler.h"

#include "browsertheme.h"

#include <qapplication.h>
#include <qbuffer.h>
#include <qcryptographichash.h>
#include <qdatetime.h>
#include <qdir.h>
#include <qfile.h>
#include <qfileiconprovider.h>
#include <qfileinfo.h>
#include <qhash.h>
#include <qlocale.h>
#include <qstyle.h>
#include <qurl.h>
#include <qwebengineprofile.h>
#include <qwebenginesettings.h>
#include <qwebengineurlrequestjob.h>

FileAccessHandler::FileAccessHandler(QObject *parent)
    : SchemeAccessHandler(parent)
{
}

QByteArray FileAccessHandler::scheme() const
{
    return schemeName();
}

QByteArray FileAccessHandler::schemeName()
{
    return QByteArrayLiteral("arora-file");
}

QUrl FileAccessHandler::urlForLocalPath(const QString &path)
{
    QUrl url;
    url.setScheme(QString::fromLatin1(schemeName()));
    url.setPath(path);
    return url;
}

void FileAccessHandler::requestStarted(QWebEngineUrlRequestJob *job)
{
    if (job->requestMethod() != "GET") {
        job->fail(QWebEngineUrlRequestJob::UrlInvalid);
        return;
    }

    // SEC02: this handler enumerates the local filesystem, so only the
    // browser itself (empty initiator) and other local pages may use
    // it — a remote http/https initiator could otherwise read directory
    // listings through an iframe or a poked navigation.
    const QString initiatorScheme = job->initiator().scheme();
    if (!initiatorScheme.isEmpty()
        && initiatorScheme != QLatin1String("file")
        && initiatorScheme != QLatin1String("arora-file")
        && initiatorScheme != QLatin1String("qrc")) {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
    }

    // This handler only lists directories; a regular file URL should
    // never reach it because WebPage only redirects directories.
    const QString path = job->requestUrl().path();
    if (!QFileInfo(path).isDir()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }

    // requestStarted() runs on the IO thread, but building the listing
    // needs QFileIconProvider/QStyle pixmaps, so it is queued onto the
    // GUI thread that owns this handler.
    QMetaObject::invokeMethod(this, [this, job]() {
        replyToJob(job);
    }, Qt::QueuedConnection);
}

static QString cssLinkClass(const QIcon &icon, int size = 32)
{
    // The CSS class generation is a bit tricky, because QIcon/QPixmap's
    // cacheKey() returns the different values for the same icons on my Windows
    // box (tested with XP). Thus, the checksum of the actual text of the CSS class
    // is used for the class name.
    QString data = QLatin1String("a.%3 {\n\
  padding-left: %1px;\n\
  background: transparent url(data:image/png;base64,%2) no-repeat center left;\n\
  font-weight: bold;\n\
}\n");
    QPixmap pixmap = icon.pixmap(QSize(size, size));
    QBuffer imageBuffer;
    imageBuffer.open(QBuffer::ReadWrite);
    if (!pixmap.save(&imageBuffer, "PNG")) {
        // If an error occured, write a blank pixmap
        pixmap = QPixmap(size, size);
        pixmap.fill(Qt::transparent);
        imageBuffer.buffer().clear();
        pixmap.save(&imageBuffer, "PNG");
    }
    return data.arg(size+4).arg(QLatin1String(imageBuffer.buffer().toBase64()));
}

void FileAccessHandler::replyToJob(QPointer<QWebEngineUrlRequestJob> job)
{
    if (!job)
        return;

    QDir dir(job->requestUrl().path());
    if (!dir.exists()) {
        job->fail(QWebEngineUrlRequestJob::UrlNotFound);
        return;
    }
    if (!dir.isReadable()) {
        job->fail(QWebEngineUrlRequestJob::RequestDenied);
        return;
    }

    // Format a html page for the directory contents
    QFile dirlistFile(QLatin1String(":/dirlist.html"));
    if (!dirlistFile.open(QIODevice::ReadOnly)) {
        job->fail(QWebEngineUrlRequestJob::RequestFailed);
        return;
    }
    QString html = QString::fromUtf8(dirlistFile.readAll());
    html = html.arg(dir.absolutePath().toHtmlEscaped(), tr("Contents of %1").arg(dir.absolutePath().toHtmlEscaped()));

    // Templates for the listing
    QString link = QLatin1String("<a class=\"%1\" href=\"%2\">%3</a>");
    QString row = QLatin1String("<tr%1> <td class=\"name\">%2</td> <td class=\"size\">%3</td> <td class=\"modified\">%4</td> </tr>\n");

    QFileIconProvider iconProvider;
    QHash<QString, bool> existingClasses;
    int iconSize = QWebEngineProfile::defaultProfile()->settings()->fontSize(QWebEngineSettings::DefaultFontSize);
    QFileInfoList list = dir.entryInfoList(QDir::AllEntries | QDir::Hidden, QDir::Name | QDir::DirsFirst);
    QString dirlist, classes;

    // Write link to parent directory first
    if (!dir.isRoot()) {
        QIcon icon = qApp->style()->standardIcon(QStyle::SP_FileDialogToParent);
        classes += cssLinkClass(icon, iconSize).arg(QLatin1String("link_parent"));

        QString addr = urlForLocalPath(QFileInfo(dir.absoluteFilePath(QLatin1String(".."))).canonicalFilePath()).toString();
        QString size, modified; // Empty by intention
        dirlist += row.arg(QString()).arg(link.arg(QLatin1String("link_parent")).arg(addr).arg(QLatin1String(".."))).arg(size).arg(modified);
    }

    for (int i = 0; i < list.count(); ++i) {
        // Skip '.' and '..'
        if (list[i].fileName() == QLatin1String(".") || list[i].fileName() == QLatin1String("..")) {
            continue;
        }

        // Fetch file icon and generate a corresponding CSS class if neccessary
        QIcon icon = iconProvider.icon(list[i]);
        QString cssClass = cssLinkClass(icon, iconSize);
        QByteArray cssData = cssClass.toLatin1();
        QString className = QString(QLatin1String("link_%1")).arg(QLatin1String(QCryptographicHash::hash(cssData, QCryptographicHash::Md4).toHex()));
        if (!existingClasses.contains(className)) {
            classes += cssClass.arg(className);
            existingClasses.insert(className, true);
        }

        QString addr = list[i].isDir()
                ? urlForLocalPath(list[i].canonicalFilePath()).toString()
                : QString::fromUtf8(QUrl::fromLocalFile(list[i].canonicalFilePath()).toEncoded());
        QString size, modified;
        if (list[i].isFile())
            size = tr("%1 KB").arg(QString::number(list[i].size()/1024));
        modified = QLocale().toString(list[i].lastModified(), QLocale::ShortFormat);

        QString classes;
        if (list[i].isHidden())
            classes = QLatin1String(" class=\"hidden\"");
        dirlist += row.arg(classes).arg(link.arg(className).arg(addr).arg(list[i].fileName().toHtmlEscaped())).arg(size).arg(modified);
    }

    html = html.arg(classes).arg(dirlist).arg(tr("Show Hidden Files"));
    // THEME01: follow the chrome palette — the listing is generated
    // chrome, not web content.
    BrowserTheme::decorateInternalPage(html);

    // The job takes ownership of the buffer.
    QBuffer *buffer = new QBuffer(job);
    buffer->setData(html.toUtf8());
    buffer->open(QIODevice::ReadOnly);
    job->reply("text/html", buffer);
}
