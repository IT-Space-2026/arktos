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

#include "printpreviewdialog.h"

#include "pdfview.h"

#include <qdialogbuttonbox.h>
#include <qdir.h>
#include <qfiledialog.h>
#include <qfile.h>
#include <qicon.h>
#include <qlabel.h>
#include <qmessagebox.h>
#include <qpagesetupdialog.h>
#include <qprinter.h>
#include <qtemporarydir.h>
#include <qtimer.h>
#include <qtoolbar.h>
#include <qboxlayout.h>
#include <qwebenginepage.h>

PrintPreviewDialog::PrintPreviewDialog(QWidget *parent)
    : QDialog(parent)
    , m_pdfView(new PdfView(this))
    , m_statusLabel(new QLabel(this))
    , m_pageSetupAction(nullptr)
    , m_saveAction(nullptr)
    , m_printer(new QPrinter(QPrinter::HighResolution))
{
    setWindowTitle(tr("Print"));
    resize(900, 700);

    QToolBar *bar = new QToolBar(this);
    bar->setFloatable(false);
    bar->setMovable(false);
    // The bundled themes carry no page-setup icon — text keeps the
    // actions readable either way.
    bar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    // PRINT02: this dialog IS the print dialog — the primary action
    // leads the toolbar.  It runs the native print pipeline on the
    // generated PDF (QPrintDialog -> PdfCanvas::printPages).
    bar->addAction(
        QIcon::fromTheme(QStringLiteral("document-print")),
        tr("Print..."), m_pdfView, &PdfView::printDocument);
    m_pageSetupAction = bar->addAction(
        tr("Page Setup..."), this, &PrintPreviewDialog::showPageSetup);
    m_pageSetupAction->setVisible(false);   // setPage() reveals it
    m_saveAction = bar->addAction(
        QIcon::fromTheme(QStringLiteral("document-save-as")),
        tr("Save as PDF..."), this, &PrintPreviewDialog::saveCopy);

    QWidget *spacer = new QWidget(bar);
    spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    bar->addWidget(spacer);
    bar->addWidget(m_statusLabel);

    QDialogButtonBox *buttons =
        new QDialogButtonBox(QDialogButtonBox::Close, this);
    connect(buttons, &QDialogButtonBox::rejected,
            this, &QDialog::reject);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(bar);
    layout->addWidget(m_pdfView, 1);
    layout->addWidget(buttons);

    // The document view's reload action is the preview's
    // "regenerate" (page mode) or "re-read the file" (document mode).
    connect(m_pdfView, &PdfView::reloadRequested,
            this, [this](const QUrl &) { regenerate(); });
    connect(m_pdfView, &PdfView::documentReady,
            this, [this]() {
        setStatus(QString());
        emit previewReady();
    });
    connect(m_pdfView, &PdfView::documentFailed,
            this, [this](const QString &message) { fail(message); });
}

PrintPreviewDialog::~PrintPreviewDialog()
{
    delete m_printer;
    delete m_tempDir;
}

void PrintPreviewDialog::setDocumentFile(const QString &pdfPath,
                                         const QString &title)
{
    m_fixedPath = pdfPath;
    if (!title.isEmpty())
        setWindowTitle(tr("Print — %1").arg(title));
    m_pdfView->setDocument(pdfPath, QUrl());
}

void PrintPreviewDialog::setPage(QWebEnginePage *page,
                                 const QUrl &sourceUrl)
{
    if (m_page)     // one page binding per dialog
        return;
    m_page = page;
    m_sourceUrl = sourceUrl;
    if (!m_page) {
        fail(tr("The page is gone — there is nothing to preview."));
        return;
    }
    m_tempDir = new QTemporaryDir(
        QDir::temp().filePath(QStringLiteral("arora-print-preview-XXXXXX")));
    if (!m_tempDir->isValid()) {
        delete m_tempDir;
        m_tempDir = nullptr;
        fail(tr("Cannot create a scratch directory."));
        m_page.clear();
        return;
    }
    connect(m_page, &QWebEnginePage::pdfPrintingFinished,
            this, [this](const QString &filePath, bool success) {
        if (filePath != m_pdfPath)
            return;     // a superseded generation finished late
        if (!success) {
            // QtWebEngine serializes print jobs per page: a printToPdf
            // issued while a previous one is still in flight — e.g. a
            // job orphaned by a preview the user just closed — is
            // rejected outright.  The busy window is short, so a
            // bounded retry rides it out; a genuinely dead page
            // exhausts the retries and fails honestly.
            if (m_retries < 3) {
                ++m_retries;
                QTimer::singleShot(250, this,
                                   &PrintPreviewDialog::regenerate);
                return;
            }
            fail(tr("Could not generate the preview."));
            return;
        }
        m_pdfView->setDocument(m_pdfPath, m_sourceUrl);
    });
    m_pageSetupAction->setVisible(true);
    regenerate();
}

bool PrintPreviewDialog::isReady() const
{
    return m_pdfView->isReady();
}

void PrintPreviewDialog::applyPageLayout(const QPageLayout &layout)
{
    m_printer->setPageLayout(layout);
    regenerate();
}

void PrintPreviewDialog::regenerate()
{
    if (!m_page) {
        // Document mode: "reload" re-reads the same file.
        if (!m_fixedPath.isEmpty())
            m_pdfView->setDocument(m_fixedPath, QUrl());
        else
            setStatus(tr("The page is gone — there is nothing to "
                         "preview."));
        return;
    }
    // A fresh name per generation keeps a late pdfPrintingFinished
    // from an earlier request from being mistaken for the latest.
    m_pdfPath = m_tempDir->filePath(
        QStringLiteral("preview-%1.pdf").arg(++m_generation));
    m_failed = false;
    m_failureMessage.clear();
    setStatus(tr("Generating preview..."));
    m_page->printToPdf(m_pdfPath, m_printer->pageLayout());
}

void PrintPreviewDialog::showPageSetup()
{
    QPageSetupDialog dialog(m_printer, this);
    if (dialog.exec() == QDialog::Accepted)
        regenerate();
}

void PrintPreviewDialog::saveCopy()
{
    const QString source =
        !m_fixedPath.isEmpty() ? m_fixedPath : m_pdfPath;
    if (!m_pdfView->isReady() || source.isEmpty()
            || !QFile::exists(source)) {
        QMessageBox::information(this, tr("Save as PDF"),
            tr("The preview is not ready yet."));
        return;
    }
    QString suggested = m_pdfView->documentTitle();
    if (suggested.isEmpty())
        suggested = QStringLiteral("page");
    if (!suggested.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
        suggested += QLatin1String(".pdf");
    QString target = QFileDialog::getSaveFileName(this,
        tr("Save as PDF"), suggested, tr("PDF files (*.pdf)"));
    if (target.isEmpty())
        return;
    if (!target.endsWith(QLatin1String(".pdf"), Qt::CaseInsensitive))
        target += QLatin1String(".pdf");
    QFile::remove(target);
    if (!QFile::copy(source, target))
        QMessageBox::warning(this, tr("Save as PDF"),
            tr("Cannot write '%1'.").arg(target));
}

void PrintPreviewDialog::setStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

void PrintPreviewDialog::fail(const QString &message)
{
    m_failed = true;
    m_failureMessage = message;
    setStatus(message);
    emit previewFailed(message);
}
