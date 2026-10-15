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

#ifndef PRINTPREVIEWDIALOG_H
#define PRINTPREVIEWDIALOG_H

// PRINT01 — the print-preview replacement for QPrintPreviewDialog.
// PRINT02 — this is now the browser's ONLY print surface: the single
// File-menu "Print..." entry (Ctrl+P) and JS window.print() all open
// it, and its Print action performs the final native print — there
// is no "print without preview" path (Firefox's model).
//
// The stock dialog's paintRequested contract is synchronous painting
// into a QPrinter paint device, which engine printing cannot satisfy
// (QWebEnginePage::printToPdf is asynchronous) — the preview shipped
// blank with "1 / 0" pages.  This dialog instead asks the page to
// generate a real PDF — true pagination and margins, the same bytes
// a print would produce — and renders it through the shell's own
// PDF stack (PdfDocumentClient + PdfCanvas, PDF02's hayro rasterizer).
// The toolbar keeps the stock UX: Print (native QPrintDialog — full
// printer selection/copies/page range — then PdfCanvas::printPages),
// Page Setup (regenerates under the chosen layout), Save as PDF,
// page navigation, zoom and fit-width.

#include <qdialog.h>
#include <qpagelayout.h>
#include <qpointer.h>
#include <qurl.h>

class QAction;
class QLabel;
class PdfView;
class QPrinter;
class QTemporaryDir;
class QWebEnginePage;

class PrintPreviewDialog : public QDialog
{
    Q_OBJECT

public:
    explicit PrintPreviewDialog(QWidget *parent = nullptr);
    ~PrintPreviewDialog() override;

    // Preview a PDF already on disk — a tab hosting the shell
    // document view.  The document's pagination is fixed, so the
    // Page Setup action stays hidden.
    void setDocumentFile(const QString &pdfPath, const QString &title);

    // Preview a live engine page: printToPdf generates the document
    // asynchronously and Page Setup / reload regenerate it under the
    // current layout.  Call once — the QPointer guard lets the tab
    // die mid-generation without consequence.
    void setPage(QWebEnginePage *page, const QUrl &sourceUrl);

    // Diagnostics for --printpreview-smoke / autotests: the embedded
    // document view, whether a document has loaded, and whether
    // generation has already failed (signals can be missed by a
    // poller that starts after setPage).
    PdfView *pdfView() const { return m_pdfView; }
    bool isReady() const;
    bool hasFailed() const { return m_failed; }
    QString failureMessage() const { return m_failureMessage; }

    // Applies a layout the way the Page Setup dialog would and
    // regenerates — tests drive this directly instead of the modal
    // QPageSetupDialog.
    void applyPageLayout(const QPageLayout &layout);

signals:
    void previewReady();
    void previewFailed(const QString &message);

private slots:
    void regenerate();
    void showPageSetup();
    void saveCopy();

private:
    void setStatus(const QString &text);
    void fail(const QString &message);

    QPointer<QWebEnginePage> m_page;
    QUrl m_sourceUrl;
    PdfView *m_pdfView;
    QLabel *m_statusLabel;
    QAction *m_pageSetupAction;
    QAction *m_saveAction;
    QTemporaryDir *m_tempDir = nullptr;
    QString m_pdfPath;      // generated file (page mode)
    QString m_fixedPath;    // setDocumentFile mode
    QPrinter *m_printer;    // owns the preview page layout
    bool m_failed = false;
    QString m_failureMessage;
    int m_generation = 0;
    int m_retries = 0;
};

#endif // PRINTPREVIEWDIALOG_H
