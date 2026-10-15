/*
 * Copyright 2008 Christian Franke <cfchris6@ts2server.com>
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

#ifndef SOURCEVIEWER_H
#define SOURCEVIEWER_H

#include <qdialog.h>
#include <qbytearray.h>
#include <qelapsedtimer.h>
#include <qstring.h>
#include <qurl.h>

class QVBoxLayout;
class SourceHighlighter;
class PlainTextEditSearch;
class QPlainTextEdit;
class QMenuBar;
class QMenu;
class QAction;
class QNetworkReply;
class SourceViewer : public QDialog
{
    Q_OBJECT

public:
    /* The constructor starts the wire re-fetch immediately — the
       caller kicks the page's toHtml() at the same time and hands the
       serialization over whenever it lands, so the two stages run in
       parallel instead of one after another. */
    SourceViewer(const QString &title, const QUrl &url,
                 QWidget *parent = nullptr);
    ~SourceViewer();

    void setDomSource(const QString &markup);
    bool isResolved() const;

private:
    void maybeResolve();
    void probeWireSource();
    void showSource(const QString &text, const char *decision);

    QPlainTextEdit *m_edit;
    SourceHighlighter *m_highlighter;
    PlainTextEditSearch *m_plainTextEditSearch;
    QVBoxLayout *m_layout;
    QMenuBar *m_menuBar;
    QMenu *m_editMenu;
    QAction *m_findAction;
    QMenu *m_viewMenu;
    QNetworkReply *m_reply;
    QString m_source;
    QByteArray m_wire;
    QString m_wireMime;
    QUrl m_wireUrl;
    bool m_fetchDone;
    bool m_fetchOk;
    bool m_domDone;
    bool m_resolved;
    QElapsedTimer m_clock;
    qint64 m_fetchMs;
    qint64 m_domMs;
    qint64 m_probeStartMs;

private slots:
    void loadingFinished();
};

#endif
