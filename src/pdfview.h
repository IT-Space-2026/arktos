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

#ifndef PDFVIEW_H
#define PDFVIEW_H

// PDF02: the shell-owned, engine-neutral PDF document view.
//
// QtWebEngine's built-in viewer is a Chromium-embedded PDFium surface
// and libservo has no PDF viewer at all — a document view owned by
// the shell is the only shape that serves every backend.  The pieces
// here deliberately contain no QtWebEngine types:
//
//   * PdfDocumentClient owns one rasterization session — normally the
//     `arora --pdf-worker` subprocess under the bwrap allowlist
//     (staged PDF read-only, per-view work dir, no network, no $HOME)
//     speaking the JSONL protocol in pdfworker.h; on hosts without a
//     confinement backend it degrades to the same rustcore calls
//     in-process and says so through backendName().  ARORA_PDF_INPROC=1
//     forces the in-process path for tests.
//   * PdfCanvas lays out every page vertically (like a continuous
//     document, the way evince/okular scroll), paints the rendered
//     rasters and drives zoom/find/print.
//   * PdfView is the chrome: toolbar (page nav, zoom, find, print)
//     wrapped around the canvas, embeddable anywhere a QWidget goes —
//     WebViewWithSearch stacks it over the WebView and EngineTab can
//     host the identical widget, so the view genuinely is shared
//     across backends.
//
// Every document reaching here has already passed the PDF01
// sanitize rewrite — the pipeline stages sanitized bytes and only
// sanitized bytes; the shell view never sees a raw remote PDF.

#include <qabstractscrollarea.h>
#include <qhash.h>
#include <qimage.h>
#include <qlist.h>
#include <qpointer.h>
#include <qprocess.h>
#include <qrect.h>
#include <qset.h>
#include <qsize.h>
#include <qurl.h>
#include <qwidget.h>

class PdfDocumentClient;
class QComboBox;
class QKeyEvent;
class QLabel;
class QLineEdit;
class QMouseEvent;
class QPaintEvent;
class QPrinter;
class QProcess;
class QPushButton;
class QResizeEvent;
class QSpinBox;
class QTemporaryDir;
class QTimer;
class QToolBar;
class QWheelEvent;

// ------------------------------------------------------------------
// One document session: metadata, page rasters, text search.  Async
// by contract — the in-process backend emits the same signals.
class PdfDocumentClient : public QObject
{
    Q_OBJECT

public:
    explicit PdfDocumentClient(QObject *parent = nullptr);
    ~PdfDocumentClient() override;

    // Starts the session (worker spawn or in-process open).  Result
    // arrives as ready()/failed(); until ready() the getters below
    // are empty.
    void open(const QString &pdfPath);
    bool isReady() const { return m_ready; }
    // "bwrap-worker" | "in-process" — surfaced so tests and the
    // status line can tell confined from degraded operation.
    QString backendName() const { return m_backend; }

    int pageCount() const { return m_pageSizes.size(); }
    // Page dimensions in points (1/72in); invalid when out of range.
    QSizeF pageSize(int page) const;
    QString title() const { return m_title; }

    // Request a page raster — PNG decoded parent-side, emitted on
    // pageRendered().  Requests are deduplicated caller-side.
    qint64 renderPage(int page, double scale);
    // Page-granular search: returns the request id; findResult()
    // carries the hit page (-1 on no match) and the document-wide
    // occurrence total.
    qint64 findText(const QString &needle, int fromPage, int dir);
    // Synchronous raster for print(): blocks up to timeoutMs.
    QImage renderPageSync(int page, double scale, int timeoutMs = 30000);

    // Smoke/self-test hook: spawn one worker with probe_paths and
    // collect its filesystem visibility report — verifies the
    // allowlist really hides $HOME from inside the sandbox.
    static bool probeWorker(const QStringList &paths,
                            QHash<QString, bool> *result,
                            QString *error,
                            int timeoutMs = 30000);

    void abort();

signals:
    void ready();
    void failed(const QString &message);
    void pageRendered(qint64 request, const QImage &image);
    void renderFailed(qint64 request, const QString &message);
    void findResult(qint64 request, int page, int total);

private:
    bool startWorker(const QString &pdfPath);
    void sendCommand(const QByteArray &jsonLine);
    void handleWorkerOutput();
    void workerDead(const QString &message);
    bool openInProcess(const QString &pdfPath);

    QProcess *m_process = nullptr;
    QTemporaryDir *m_workDir = nullptr;
    QByteArray m_stdoutBuffer;
    QByteArray m_stderrBuffer;
    QList<QSizeF> m_pageSizes;
    QSet<qint64> m_pending;             // in-flight worker requests
    QString m_title;
    QString m_backend;
    bool m_ready = false;
    bool m_failed = false;
    qint64 m_nextRequest = 1;
#ifdef ARORA_RUSTCORE
    quintptr m_doc = 0;
#endif
};

// ------------------------------------------------------------------
// Continuous-document canvas: pages stacked vertically, centered,
// rendered on demand at the current zoom.
class PdfCanvas : public QAbstractScrollArea
{
    Q_OBJECT

public:
    explicit PdfCanvas(QWidget *parent = nullptr);

    void setClient(PdfDocumentClient *client);   // not owned
    void clearDocument();

    int pageCount() const { return m_pageRects.size(); }
    int currentPage() const;
    double zoom() const { return m_zoom; }
    bool fitWidth() const { return m_fitWidth; }

    void goToPage(int page);
    void setZoom(double zoom);       // 0.25 .. 8.0
    void setFitWidth(bool on);
    void find(const QString &needle, int dir);
    // Paints every page through the client at ~150dpi — the print
    // handoff needs no engine, the rasters ARE the print source.
    bool printPages(QPrinter *printer, QString *error);

signals:
    void currentPageChanged(int page);
    void zoomChanged(double zoom);
    // Short status strings for the find row ("5 hits" / "no match").
    void findStatus(const QString &text);

protected:
    void paintEvent(QPaintEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

private:
    void relayout();
    QRectF pageRect(int page) const;      // content coordinates
    void requestPage(int page);
    void handleRendered(qint64 request, const QImage &image);
    void handleFindResult(qint64 request, int page, int total);
    void updateScrollbars();

    QPointer<PdfDocumentClient> m_client;
    QList<QSizeF> m_sizesPt;              // points
    QList<QRectF> m_pageRects;            // content coords
    QHash<int, QImage> m_images;
    QHash<qint64, int> m_requests;        // req id -> page
    double m_zoom = 1.0;
    bool m_fitWidth = false;
    int m_currentPage = 0;
    QString m_needle;
    int m_findPage = -1;                  // highlighted hit page, -1 none
    bool m_dragging = false;
    QPoint m_dragStart;
    QPoint m_dragScroll;
};

// ------------------------------------------------------------------
// Chrome around the canvas — toolbar (navigation, zoom, fit, reload,
// print) + a find row, shown/hidden like WebViewSearch.
class PdfView : public QWidget
{
    Q_OBJECT

public:
    explicit PdfView(QWidget *parent = nullptr);
    ~PdfView() override;

    // Attach a staged (already-sanitized) PDF.  sourceUrl is kept for
    // the reload action and window title — the bytes on disk are the
    // document.
    void setDocument(const QString &localPath, const QUrl &sourceUrl);
    void clearDocument();

    QUrl sourceUrl() const { return m_sourceUrl; }
    QString localPath() const { return m_localPath; }
    QString documentTitle() const;
    bool isReady() const;
    // Diagnostics for --pdfview-smoke: which rasterization backend
    // served the document.
    QString backendName() const;
    PdfDocumentClient *client() const { return m_client; }
    PdfCanvas *canvas() const { return m_canvas; }

    // Print handoff — public so the window's Print action reaches it
    // when this tab is the active document (BrowserMainWindow routes
    // here instead of the engine print path).
    void printDocument();

signals:
    // The host navigates the tab back to the document's origin —
    // which re-enters the sanitize pipeline, keeping reload honest.
    void reloadRequested(const QUrl &sourceUrl);
    void documentReady();
    void documentFailed(const QString &message);
    void titleChanged(const QString &title);

protected:
    void keyPressEvent(QKeyEvent *event) override;

private slots:
    void handleClientReady();
    void updatePageControls();
    void syncZoomSpin(double zoom);
    void showFindBar();
    void findNext();
    void findPrevious();

private:
    PdfDocumentClient *m_client;
    PdfCanvas *m_canvas;
    QToolBar *m_toolbar;
    QWidget *m_findBar;
    QLineEdit *m_findEdit;
    QLabel *m_findStatus;
    QSpinBox *m_pageSpin;
    QLabel *m_pageCount;
    QComboBox *m_zoomBox;
    bool m_spinSync = false;            // page-spin echo suppression
    QString m_localPath;
    QUrl m_sourceUrl;
};

#endif // PDFVIEW_H
