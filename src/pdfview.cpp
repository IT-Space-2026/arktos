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

#include "pdfview.h"

#include "bwrapgenerator.h"
#include "pdfsupport.h"
#include "sandboxmanager.h"

#include <qaction.h>
#include <qcombobox.h>
#include <qcoreapplication.h>
#include <qeventloop.h>
#include <qfile.h>
#include <qfileinfo.h>
#include <qboxlayout.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qevent.h>
#include <qlabel.h>
#include <qlineedit.h>
#include <qmargins.h>
#include <qmessagebox.h>
#include <qpainter.h>
#include <qpalette.h>
#include <qprintdialog.h>
#include <qprinter.h>
#include <qpushbutton.h>
#include <qscrollbar.h>
#include <qspinbox.h>
#include <qtemporarydir.h>
#include <qtimer.h>
#include <qtoolbar.h>
#include <qtoolbutton.h>
#include <qurl.h>


#include <limits>
#include <utility>

#ifdef ARORA_RUSTCORE
#include "rustcore.h"
#endif

// 96dpi logical pixels per point at 100% — the usual CSS-point
// convention document viewers use, so 100% reads at natural size.
static constexpr double kBaseScale = 96.0 / 72.0;
// Page spacing/shadow margins in logical pixels.
static constexpr int kPageGap = 12;
static constexpr int kPageMargin = 8;

// ==================================================================
// PdfDocumentClient — worker-spawn + protocol, or in-process FFI.
// ==================================================================

PdfDocumentClient::PdfDocumentClient(QObject *parent)
    : QObject(parent)
{
}

PdfDocumentClient::~PdfDocumentClient()
{
    abort();
}

void PdfDocumentClient::abort()
{
#ifdef ARORA_RUSTCORE
    if (m_doc) {
        rc_pdfdoc_close(m_doc);
        m_doc = 0;
    }
#endif
    if (m_process) {
        // Polite stop, then the hard kills — a rasterizer stuck in a
        // hostile page must not outlive the tab that spawned it.
        sendCommand(QByteArray("{\"cmd\":\"stop\"}\n"));
        m_process->closeWriteChannel();
        if (!m_process->waitForFinished(500)) {
            m_process->kill();
            m_process->waitForFinished(1000);
        }
        m_process->deleteLater();
        m_process = nullptr;
    }
    delete m_workDir;
    m_workDir = nullptr;
}

void PdfDocumentClient::open(const QString &pdfPath)
{
#ifdef ARORA_RUSTCORE
    const bool inProc =
        qEnvironmentVariableIntValue("ARORA_PDF_INPROC") == 1
        || SandboxManager::bwrapPath().isEmpty();
    if (inProc) {
        if (!openInProcess(pdfPath))
            return;
        // Async contract: deliver ready() on the next loop turn like
        // the worker path does.
        QTimer::singleShot(0, this, [this]() {
            if (!m_failed)
                emit ready();
        });
        return;
    }
    if (startWorker(pdfPath))
        return;
    // A spawn failure degrades to the in-process surface rather than
    // refusing the document — same honest-degrade shape as the rest
    // of the sandbox machinery.
    if (openInProcess(pdfPath))
        QTimer::singleShot(0, this, [this]() {
            if (!m_failed)
                emit ready();
        });
#else
    Q_UNUSED(pdfPath);
    m_failed = true;
    emit failed(tr("PDF rendering needs the rust core"));
#endif
}

QSizeF PdfDocumentClient::pageSize(int page) const
{
    if (page < 0 || page >= m_pageSizes.size())
        return QSizeF();
    return m_pageSizes.at(page);
}

bool PdfDocumentClient::openInProcess(const QString &pdfPath)
{
#ifdef ARORA_RUSTCORE
    QFile in(pdfPath);
    if (!in.open(QIODevice::ReadOnly)) {
        m_failed = true;
        emit failed(tr("Cannot read %1").arg(pdfPath));
        return false;
    }
    const QByteArray bytes = in.readAll();
    m_doc = rc_pdfdoc_open(
        reinterpret_cast<const uint8_t *>(bytes.constData()),
        size_t(bytes.size()));
    if (!m_doc) {
        char *msg = rc_last_error_message();
        m_failed = true;
        emit failed(QString::fromUtf8(msg ? msg : "pdf open failed"));
        rc_string_free(msg);
        return false;
    }
    const int pages = rc_pdfdoc_page_count(m_doc);
    for (int i = 0; i < pages; ++i) {
        double w = 0.0, h = 0.0;
        if (rc_pdfdoc_page_size(m_doc, i, &w, &h) == RC_OK)
            m_pageSizes.append(QSizeF(w, h));
        else
            m_pageSizes.append(QSizeF());
    }
    if (char *t = rc_pdfdoc_title(m_doc)) {
        m_title = QString::fromUtf8(t);
        rc_string_free(t);
    }
    m_backend = QStringLiteral("in-process");
    m_ready = true;
    return true;
#else
    Q_UNUSED(pdfPath);
    return false;
#endif
}

bool PdfDocumentClient::startWorker(const QString &pdfPath)
{
    const QString program = QCoreApplication::applicationFilePath();
    if (!QFileInfo(program).isExecutable())
        return false;

    // The worker's writable surface lives beside the staged document
    // under the managed pdf-view dir — nothing in $HOME beyond what
    // the bind grants explicitly.
    m_workDir = new QTemporaryDir(
        PdfSupport::tempDirPath() + QLatin1String("/w-XXXXXX"));
    if (!m_workDir->isValid()) {
        delete m_workDir;
        m_workDir = nullptr;
        return false;
    }

    const QString bwrap = SandboxManager::bwrapPath();
    QStringList args;
    QString docPath, workPath;
    if (!bwrap.isEmpty()) {
        args = BwrapGenerator::pdfWorkerCommandLine(
            program, {QStringLiteral("--pdf-worker")},
            m_workDir->path(), pdfPath);
        docPath = BwrapGenerator::pdfDocumentPath();
        workPath = BwrapGenerator::pdfWorkDirPath();
        m_backend = QStringLiteral("bwrap-worker");
    } else {
        args << program << QStringLiteral("--pdf-worker");
        docPath = pdfPath;
        workPath = m_workDir->path();
        m_backend = QStringLiteral("worker");
    }
    if (args.size() < 2)
        return false;

    m_process = new QProcess(this);
    m_process->setProgram(args.first());
    m_process->setArguments(args.mid(1));
    connect(m_process, &QProcess::readyReadStandardOutput,
            this, &PdfDocumentClient::handleWorkerOutput);
    // stderr carries bwrap's own diagnostics (mount failures,
    // loader errors) — the protocol only speaks on stdout, so an
    // early death is undiagnosable without it.
    connect(m_process, &QProcess::readyReadStandardError,
            this, [this]() {
        m_stderrBuffer += m_process->readAllStandardError();
        if (m_stderrBuffer.size() > 8192)
            m_stderrBuffer = m_stderrBuffer.right(8192);
    });
    connect(m_process, &QProcess::finished, this,
            [this](int, QProcess::ExitStatus) {
        if (!m_ready && !m_failed)
            workerDead(tr("pdf worker exited before the document opened"));
        // After ready a dead worker just means no more renders —
        // pending requests fail lazily through workerDead().
        else if (m_ready && !m_failed
                 && !m_pending.isEmpty())
            workerDead(tr("pdf worker exited"));
    });
    connect(m_process, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            workerDead(tr("pdf worker failed to start"));
    });
    m_process->start();
    if (!m_process->waitForStarted(10000)) {
        workerDead(tr("pdf worker failed to start"));
        return false;
    }

    QJsonObject job;
    job.insert(QLatin1String("cmd"), QLatin1String("job"));
    job.insert(QLatin1String("pdf"), docPath);
    job.insert(QLatin1String("work_dir"), workPath);
    sendCommand(QJsonDocument(job).toJson(QJsonDocument::Compact)
                + '\n');
    return true;
}

void PdfDocumentClient::sendCommand(const QByteArray &jsonLine)
{
    if (m_process && m_process->state() == QProcess::Running)
        m_process->write(jsonLine);
}

void PdfDocumentClient::workerDead(const QString &message)
{
    if (m_failed)
        return;
    m_failed = true;
    if (m_process)
        m_stderrBuffer += m_process->readAllStandardError();
    QString detail = message;
    const QByteArray err = m_stderrBuffer.trimmed();
    if (!err.isEmpty())
        detail += QLatin1String(" — ")
            + QString::fromLocal8Bit(err.split('\n').last());
    emit failed(detail);
}

void PdfDocumentClient::handleWorkerOutput()
{
    if (!m_process)
        return;
    m_stdoutBuffer += m_process->readAllStandardOutput();
    int nl;
    while ((nl = m_stdoutBuffer.indexOf('\n')) >= 0) {
        const QByteArray line = m_stdoutBuffer.left(nl).trimmed();
        m_stdoutBuffer.remove(0, nl + 1);
        if (line.isEmpty())
            continue;
        const QJsonObject ev = QJsonDocument::fromJson(line).object();
        const QString kind = ev.value(QLatin1String("ev")).toString();
        if (kind == QLatin1String("meta")) {
            const int pages = ev.value(QLatin1String("pages")).toInt();
            const QJsonArray sizes =
                ev.value(QLatin1String("sizes")).toArray();
            m_pageSizes.clear();
            for (int i = 0; i < pages && i < sizes.size(); ++i) {
                const QJsonArray wh = sizes.at(i).toArray();
                m_pageSizes.append(
                    QSizeF(wh.at(0).toDouble(),
                           wh.at(1).toDouble()));
            }
            m_title = ev.value(QLatin1String("title")).toString();
            m_ready = true;
            emit ready();
        } else if (kind == QLatin1String("page")) {
            const qint64 req = qint64(ev.value(QLatin1String("req")).toDouble());
            const QString file =
                ev.value(QLatin1String("file")).toString();
            m_pending.remove(req);
            // The png lands in the host-side work dir; the sandbox
            // binds it at /arora-rt/work but the parent reads its own
            // path.
            const QString path = m_workDir
                ? m_workDir->path() + QLatin1Char('/') + file
                : QString();
            QImage image(path, "PNG");
            QFile::remove(path);
            if (image.isNull())
                emit renderFailed(req, tr("page render unreadable"));
            else
                emit pageRendered(req, image);
        } else if (kind == QLatin1String("find")) {
            const qint64 req = qint64(ev.value(QLatin1String("req")).toDouble());
            m_pending.remove(req);
            emit findResult(req,
                ev.value(QLatin1String("page")).toInt(-1),
                ev.value(QLatin1String("total")).toInt(0));
        } else if (kind == QLatin1String("error")) {
            const qint64 req = qint64(ev.value(QLatin1String("req")).toDouble(-1));
            const QString message =
                ev.value(QLatin1String("message")).toString();
            if (req >= 0 && m_pending.remove(req))
                emit renderFailed(req, message);
            else if (!m_ready)
                workerDead(message);
            else
                emit renderFailed(req, message);
        }
        // "probe" events are only used by the synchronous probeWorker.
    }
}

qint64 PdfDocumentClient::renderPage(int page, double scale)
{
#ifdef ARORA_RUSTCORE
    const qint64 req = m_nextRequest++;
    if (m_doc) {
        RcBuffer png = { nullptr, 0 };
        const RcStatus status = rc_pdfdoc_render_png(
            m_doc, page, scale, &png);
        if (status != RC_OK || !png.data) {
            char *msg = rc_last_error_message();
            emit renderFailed(req,
                QString::fromUtf8(msg ? msg : "render failed"));
            rc_string_free(msg);
            return req;
        }
        QImage image = QImage::fromData(
            QByteArray::fromRawData(
                reinterpret_cast<const char *>(png.data),
                int(png.len)), "PNG");
        rc_buffer_free(png);
        emit pageRendered(req, image);
        return req;
    }
    if (m_process) {
        m_pending.insert(req);
        const QByteArray line =
            "{\"cmd\":\"render\",\"req\":" + QByteArray::number(req)
            + ",\"page\":" + QByteArray::number(page)
            + ",\"scale\":" + QByteArray::number(scale, 'g', 8)
            + "}\n";
        sendCommand(line);
        return req;
    }
#endif
    Q_UNUSED(page);
    Q_UNUSED(scale);
    return -1;
}

qint64 PdfDocumentClient::findText(const QString &needle,
                                   int fromPage, int dir)
{
#ifdef ARORA_RUSTCORE
    const qint64 req = m_nextRequest++;
    if (m_doc) {
        const QByteArray n = needle.toUtf8();
        int32_t total = 0;
        const int32_t hit = rc_pdfdoc_find(
            m_doc, n.constData(), fromPage, dir, &total);
        emit findResult(req, hit, total);
        return req;
    }
    if (m_process) {
        m_pending.insert(req);
        const QJsonObject cmd{
            {QLatin1String("cmd"), QLatin1String("find")},
            {QLatin1String("req"), double(req)},
            {QLatin1String("needle"), needle},
            {QLatin1String("from"), fromPage},
            {QLatin1String("dir"), dir},
        };
        sendCommand(QJsonDocument(cmd).toJson(QJsonDocument::Compact)
                    + '\n');
        return req;
    }
#endif
    Q_UNUSED(needle);
    Q_UNUSED(fromPage);
    Q_UNUSED(dir);
    return -1;
}

QImage PdfDocumentClient::renderPageSync(int page, double scale,
                                         int timeoutMs)
{
#ifdef ARORA_RUSTCORE
    if (m_doc) {
        RcBuffer png = { nullptr, 0 };
        if (rc_pdfdoc_render_png(m_doc, page, scale, &png) != RC_OK
                || !png.data)
            return QImage();
        QImage image = QImage::fromData(
            QByteArray::fromRawData(
                reinterpret_cast<const char *>(png.data),
                int(png.len)), "PNG");
        rc_buffer_free(png);
        return image;
    }
#endif
    const qint64 req = renderPage(page, scale);
    if (req < 0)
        return QImage();
    QImage result;
    QString failure;
    QEventLoop loop;
    QTimer timeout;
    timeout.setSingleShot(true);
    connect(&timeout, &QTimer::timeout, &loop, &QEventLoop::quit);
    connect(this, &PdfDocumentClient::pageRendered, &loop,
            [&](qint64 done, const QImage &image) {
        if (done == req) {
            result = image;
            loop.quit();
        }
    });
    connect(this, &PdfDocumentClient::renderFailed, &loop,
            [&](qint64 done, const QString &message) {
        if (done == req) {
            failure = message;
            loop.quit();
        }
    });
    timeout.start(timeoutMs);
    loop.exec();
    Q_UNUSED(failure);
    return result;
}

bool PdfDocumentClient::probeWorker(const QStringList &paths,
                                    QHash<QString, bool> *result,
                                    QString *error,
                                    int timeoutMs)
{
    if (result)
        result->clear();
    const QString program = QCoreApplication::applicationFilePath();
    if (!QFileInfo(program).isExecutable()) {
        if (error)
            *error = QStringLiteral("no arora binary");
        return false;
    }

    QTemporaryDir workDir;
    QStringList args;
    QString docPath, workPath;
    const QString bwrap = SandboxManager::bwrapPath();
    if (!bwrap.isEmpty()) {
        args = BwrapGenerator::pdfWorkerCommandLine(
            program, {QStringLiteral("--pdf-worker")},
            workDir.path(), QStringLiteral("/nonexistent.pdf"));
        docPath = BwrapGenerator::pdfDocumentPath();
        workPath = BwrapGenerator::pdfWorkDirPath();
    } else {
        args << program << QStringLiteral("--pdf-worker");
        docPath = QStringLiteral("/nonexistent.pdf");
        workPath = workDir.path();
    }

    QProcess process;
    process.setProgram(args.first());
    process.setArguments(args.mid(1));
    process.start();
    if (!process.waitForStarted(10000)) {
        if (error)
            *error = QStringLiteral("worker failed to start");
        return false;
    }

    QJsonObject job;
    job.insert(QLatin1String("cmd"), QLatin1String("job"));
    job.insert(QLatin1String("pdf"), docPath);
    job.insert(QLatin1String("work_dir"), workPath);
    job.insert(QLatin1String("probe_paths"),
               QJsonArray::fromStringList(paths));
    process.write(QJsonDocument(job).toJson(QJsonDocument::Compact)
                  + '\n');
    // The document path deliberately does not exist: the worker emits
    // its probes, fails the open and exits — the probes are the whole
    // point here.
    process.closeWriteChannel();
    process.waitForFinished(timeoutMs);
    if (process.state() == QProcess::Running) {
        process.kill();
        process.waitForFinished(1000);
        if (error)
            *error = QStringLiteral("probe timed out");
        return false;
    }

    const QByteArray out = process.readAllStandardOutput();
    const QList<QByteArray> lines = out.split('\n');
    int seen = 0;
    for (const QByteArray &line : lines) {
        if (line.isEmpty())
            continue;
        const QJsonObject ev =
            QJsonDocument::fromJson(line).object();
        if (ev.value(QLatin1String("ev")).toString()
            != QLatin1String("probe"))
            continue;
        if (result)
            result->insert(
                ev.value(QLatin1String("path")).toString(),
                ev.value(QLatin1String("readable")).toBool());
        ++seen;
    }
    if (seen == 0 && error)
        *error = QString::fromUtf8(process.readAllStandardError());
    return seen > 0;
}

// ==================================================================
// PdfCanvas — continuous-page scrollable document surface.
// ==================================================================

PdfCanvas::PdfCanvas(QWidget *parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    viewport()->setAutoFillBackground(true);
    QPalette pal = viewport()->palette();
    pal.setColor(QPalette::Window,
                 pal.color(QPalette::Dark));
    viewport()->setPalette(pal);
    setFocusPolicy(Qt::StrongFocus);
    // Scroll position -> current page tracking, cheap and timer-free.
    connect(verticalScrollBar(), &QScrollBar::valueChanged,
            this, [this](int) {
        const int page = currentPage();
        if (page != m_currentPage) {
            m_currentPage = page;
            emit currentPageChanged(page);
        }
    });
}

void PdfCanvas::setClient(PdfDocumentClient *client)
{
    if (m_client)
        disconnect(m_client, nullptr, this, nullptr);
    m_client = client;
    m_images.clear();
    m_requests.clear();
    m_sizesPt.clear();
    m_pageRects.clear();
    m_findPage = -1;
    m_needle.clear();
    if (m_client) {
        for (int i = 0; i < m_client->pageCount(); ++i)
            m_sizesPt.append(m_client->pageSize(i));
        connect(m_client, &PdfDocumentClient::pageRendered,
                this, &PdfCanvas::handleRendered);
        connect(m_client, &PdfDocumentClient::renderFailed,
                this, [this](qint64 request, const QString &) {
            // Drop the pending marker so the failed page can be
            // requested again on a later paint/zoom.
            m_requests.remove(request);
            viewport()->update();
        });
        connect(m_client, &PdfDocumentClient::findResult,
                this, &PdfCanvas::handleFindResult);
    }
    relayout();
}

void PdfCanvas::clearDocument()
{
    setClient(nullptr);
    updateScrollbars();
    viewport()->update();
}

void PdfCanvas::relayout()
{
    m_pageRects.clear();
    const qreal scale = kBaseScale * m_zoom;
    qreal y = kPageGap;
    qreal maxW = 0;
    for (const QSizeF &pt : std::as_const(m_sizesPt)) {
        const QSizeF logical(qMax(pt.width(), 1.0) * scale,
                             qMax(pt.height(), 1.0) * scale);
        m_pageRects.append(QRectF(0, y, logical.width(),
                                  logical.height()));
        maxW = qMax(maxW, logical.width());
        y += logical.height() + kPageGap;
    }
    // Horizontal centering is resolved per-viewport-width in
    // pageRect() — the stored x is a placeholder.
    Q_UNUSED(maxW);
    updateScrollbars();
    viewport()->update();
}

QRectF PdfCanvas::pageRect(int page) const
{
    if (page < 0 || page >= m_pageRects.size())
        return QRectF();
    QRectF r = m_pageRects.at(page);
    const qreal viewportW = viewport()->width();
    const qreal contentW = r.width() + 2 * kPageMargin;
    const qreal x = contentW < viewportW
        ? (viewportW - r.width()) / 2.0
        : kPageMargin;
    r.moveLeft(x - horizontalScrollBar()->value());
    r.moveTop(r.top() - verticalScrollBar()->value());
    return r;
}

void PdfCanvas::updateScrollbars()
{
    if (m_pageRects.isEmpty()) {
        verticalScrollBar()->setRange(0, 0);
        horizontalScrollBar()->setRange(0, 0);
        return;
    }
    qreal maxW = 0;
    qreal bottom = 0;
    for (const QRectF &r : std::as_const(m_pageRects)) {
        maxW = qMax(maxW, r.width() + 2 * kPageMargin);
        bottom = qMax(bottom, r.bottom() + kPageGap);
    }
    const QSize vp = viewport()->size();
    verticalScrollBar()->setRange(
        0, int(qMax(0.0, bottom - vp.height())));
    verticalScrollBar()->setPageStep(vp.height());
    verticalScrollBar()->setSingleStep(vp.height() / 10);
    horizontalScrollBar()->setRange(
        0, int(qMax(0.0, maxW - vp.width())));
    horizontalScrollBar()->setPageStep(vp.width());
    horizontalScrollBar()->setSingleStep(vp.width() / 10);
}

int PdfCanvas::currentPage() const
{
    if (m_pageRects.isEmpty())
        return 0;
    const qreal centerY = verticalScrollBar()->value()
        + viewport()->height() / 2.0;
    for (int i = 0; i < m_pageRects.size(); ++i) {
        if (m_pageRects.at(i).contains(
                QPointF(m_pageRects.at(i).center().x(), centerY)))
            return i;
    }
    // Between pages — nearest page by center distance.
    int best = 0;
    qreal bestDist = std::numeric_limits<qreal>::max();
    for (int i = 0; i < m_pageRects.size(); ++i) {
        const qreal d = qAbs(m_pageRects.at(i).center().y() - centerY);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

void PdfCanvas::goToPage(int page)
{
    if (m_pageRects.isEmpty())
        return;
    page = qBound(0, page, m_pageRects.size() - 1);
    verticalScrollBar()->setValue(
        int(m_pageRects.at(page).top() - kPageGap));
}

void PdfCanvas::setZoom(double zoom)
{
    m_zoom = qBound(0.25, zoom, 8.0);
    m_fitWidth = false;
    // Re-render everything at the new scale.
    m_images.clear();
    m_requests.clear();
    relayout();
    emit zoomChanged(m_zoom);
}

void PdfCanvas::setFitWidth(bool on)
{
    if (m_pageRects.isEmpty())
        return;
    m_fitWidth = on;
    if (on) {
        const qreal firstW = m_sizesPt.isEmpty()
            ? 1.0 : qMax(m_sizesPt.first().width(), 1.0);
        const qreal avail = viewport()->width() - 2 * kPageMargin;
        const qreal zoom = avail / (firstW * kBaseScale);
        m_zoom = qBound(0.25, zoom, 8.0);
        m_images.clear();
        m_requests.clear();
        relayout();
        emit zoomChanged(m_zoom);
    }
}

void PdfCanvas::requestPage(int page)
{
    if (!m_client || !m_client->isReady())
        return;
    if (m_images.contains(page)
            || m_requests.values().contains(page))
        return;
    // Render at zoom * devicePixelRatio so the raster is crisp on
    // high-dpi screens; the painted rect is the logical size.
    const double scale =
        kBaseScale * m_zoom * viewport()->devicePixelRatioF();
    const qint64 req = m_client->renderPage(page, scale);
    if (req >= 0)
        m_requests.insert(req, page);
}

void PdfCanvas::handleRendered(qint64 request, const QImage &image)
{
    const auto it = m_requests.find(request);
    if (it == m_requests.end())
        return;  // stale — a zoom change discarded the request
    const int page = it.value();
    m_requests.erase(it);
    if (image.isNull())
        return;
    QImage scaled = image;
    scaled.setDevicePixelRatio(viewport()->devicePixelRatioF());
    m_images.insert(page, scaled);
    viewport()->update();
}

void PdfCanvas::handleFindResult(qint64, int page, int total)
{
    if (page >= 0) {
        m_findPage = page;
        goToPage(page);
        emit findStatus(
            tr("%1 hit(s) — page %2").arg(total).arg(page + 1));
    } else {
        m_findPage = -1;
        emit findStatus(tr("No match"));
    }
    viewport()->update();
}

void PdfCanvas::find(const QString &needle, int dir)
{
    if (!m_client || !m_client->isReady() || needle.isEmpty())
        return;
    m_needle = needle;
    // A previous hit advances past its page (the core's from_page is
    // inclusive, and the view can't show two positions on one page);
    // a fresh search starts at the page the user is looking at.
    const int from = m_findPage >= 0
        ? m_findPage + (dir < 0 ? -1 : 1)
        : m_currentPage;
    m_client->findText(needle, from, dir < 0 ? -1 : 1);
}

void PdfCanvas::paintEvent(QPaintEvent *event)
{
    QPainter painter(viewport());
    painter.fillRect(event->rect(),
                     viewport()->palette().color(QPalette::Window));
    if (m_pageRects.isEmpty()) {
        painter.setPen(viewport()->palette().color(QPalette::Text));
        painter.drawText(event->rect(), Qt::AlignCenter,
                         tr("Opening document…"));
        return;
    }
    for (int i = 0; i < m_pageRects.size(); ++i) {
        const QRectF rect = pageRect(i);
        if (!rect.intersects(QRectF(event->rect())))
            continue;
        // Page shadow + paper.
        painter.fillRect(rect.translated(2, 2),
                         QColor(0, 0, 0, 60));
        painter.fillRect(rect, Qt::white);
        const auto it = m_images.constFind(i);
        if (it != m_images.constEnd()) {
            painter.drawImage(rect, *it);
        } else {
            painter.setPen(QColor(140, 140, 140));
            painter.drawText(rect, Qt::AlignCenter,
                             tr("Rendering page %1…").arg(i + 1));
            requestPage(i);
        }
        if (i == m_findPage) {
            painter.setPen(QPen(QColor(255, 165, 0), 3));
            painter.drawRect(rect.adjusted(1, 1, -1, -1));
        }
        painter.setPen(QColor(120, 120, 120));
        painter.drawRect(rect);
    }
}

void PdfCanvas::resizeEvent(QResizeEvent *event)
{
    QAbstractScrollArea::resizeEvent(event);
    if (m_fitWidth && !m_pageRects.isEmpty()) {
        // Recompute zoom against the new width — same math as
        // setFitWidth but without toggling the flag off.
        const qreal firstW = qMax(m_sizesPt.first().width(), 1.0);
        const qreal avail = viewport()->width() - 2 * kPageMargin;
        m_zoom = qBound(0.25, avail / (firstW * kBaseScale), 8.0);
        m_images.clear();
        m_requests.clear();
        relayout();
        emit zoomChanged(m_zoom);
        return;
    }
    updateScrollbars();
}

void PdfCanvas::keyPressEvent(QKeyEvent *event)
{
    const int page = m_currentPage;
    switch (event->key()) {
    case Qt::Key_PageDown:
    case Qt::Key_Space:
        if (page < m_pageRects.size() - 1)
            goToPage(page + 1);
        else
            verticalScrollBar()->setValue(
                verticalScrollBar()->maximum());
        event->accept();
        return;
    case Qt::Key_PageUp:
    case Qt::Key_Backspace:
        if (page > 0)
            goToPage(page - 1);
        else
            verticalScrollBar()->setValue(0);
        event->accept();
        return;
    case Qt::Key_Home:
        goToPage(0);
        event->accept();
        return;
    case Qt::Key_End:
        goToPage(m_pageRects.size() - 1);
        event->accept();
        return;
    case Qt::Key_Down:
        verticalScrollBar()->setValue(
            verticalScrollBar()->value()
            + verticalScrollBar()->singleStep());
        event->accept();
        return;
    case Qt::Key_Up:
        verticalScrollBar()->setValue(
            verticalScrollBar()->value()
            - verticalScrollBar()->singleStep());
        event->accept();
        return;
    case Qt::Key_Plus:
    case Qt::Key_Equal:
        setZoom(m_zoom * 1.25);
        event->accept();
        return;
    case Qt::Key_Minus:
        setZoom(m_zoom / 1.25);
        event->accept();
        return;
    default:
        break;
    }
    QAbstractScrollArea::keyPressEvent(event);
}

void PdfCanvas::wheelEvent(QWheelEvent *event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        const qreal factor =
            event->angleDelta().y() > 0 ? 1.1 : 1.0 / 1.1;
        setZoom(m_zoom * factor);
        event->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(event);
}

void PdfCanvas::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        // Drag-to-scroll — the document surface has no links or
        // selectable text, so a grab is the whole gesture.
        m_dragging = true;
        m_dragStart = event->pos();
        m_dragScroll = QPoint(horizontalScrollBar()->value(),
                              verticalScrollBar()->value());
        viewport()->setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    QAbstractScrollArea::mousePressEvent(event);
}

void PdfCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (m_dragging) {
        const QPoint delta = event->pos() - m_dragStart;
        horizontalScrollBar()->setValue(m_dragScroll.x() - delta.x());
        verticalScrollBar()->setValue(m_dragScroll.y() - delta.y());
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseMoveEvent(event);
}

void PdfCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_dragging && event->button() == Qt::LeftButton) {
        m_dragging = false;
        viewport()->unsetCursor();
        event->accept();
        return;
    }
    QAbstractScrollArea::mouseReleaseEvent(event);
}

bool PdfCanvas::printPages(QPrinter *printer, QString *error)
{
    if (!m_client || !m_client->isReady() || m_sizesPt.isEmpty()) {
        if (error)
            *error = tr("no document");
        return false;
    }
    // ~150dpi rasters keep the print sharp under the renderer's
    // 48MiB pixel cap for anything up to poster size.
    const double printScale = 150.0 / 72.0;
    QPainter painter;
    if (!painter.begin(printer)) {
        if (error)
            *error = tr("cannot start print job");
        return false;
    }
    for (int i = 0; i < m_sizesPt.size(); ++i) {
        if (i > 0 && !printer->newPage()) {
            if (error)
                *error = tr("printer refused page %1").arg(i + 1);
            return false;
        }
        const QImage image =
            m_client->renderPageSync(i, printScale);
        if (image.isNull()) {
            if (error)
                *error = tr("render of page %1 failed").arg(i + 1);
            return false;
        }
        const QRectF page = printer->pageRect(QPrinter::DevicePixel);
        const QSizeF fit = QSizeF(image.size()).scaled(
            page.size(), Qt::KeepAspectRatio);
        const QRectF target(
            (page.width() - fit.width()) / 2.0,
            (page.height() - fit.height()) / 2.0,
            fit.width(), fit.height());
        painter.drawImage(target, image);
    }
    painter.end();
    return true;
}

// Track the current page as the user scrolls — a timer-free
// connection on the scrollbar keeps this cheap.
// (wired in PdfView after construction via currentPage polling is
// avoided: we connect scrollbar valueChanged here.)

// ==================================================================
// PdfView — toolbar + find bar around the canvas.
// ==================================================================

PdfView::PdfView(QWidget *parent)
    : QWidget(parent)
    , m_client(nullptr)
    , m_canvas(new PdfCanvas(this))
    , m_toolbar(new QToolBar(this))
    , m_findBar(nullptr)
    , m_findEdit(nullptr)
    , m_findStatus(nullptr)
    , m_pageSpin(new QSpinBox(this))
    , m_pageCount(new QLabel(this))
    , m_zoomBox(new QComboBox(this))
{
    m_toolbar->setFloatable(false);
    m_toolbar->setMovable(false);
    m_toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);

    // No shortcuts on page nav — QKeySequence::Back/Forward would
    // shadow the window's history navigation while this tab has
    // focus.
    QAction *prev = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("go-previous")),
        tr("Previous page"));
    QAction *next = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("go-next")),
        tr("Next page"));
    connect(prev, &QAction::triggered, this, [this]() {
        m_canvas->goToPage(m_canvas->currentPage() - 1);
        updatePageControls();
    });
    connect(next, &QAction::triggered, this, [this]() {
        m_canvas->goToPage(m_canvas->currentPage() + 1);
        updatePageControls();
    });

    m_pageSpin->setMinimum(1);
    m_pageSpin->setMaximum(1);
    m_pageSpin->setToolTip(tr("Page"));
    m_toolbar->addWidget(m_pageSpin);
    m_toolbar->addWidget(m_pageCount);
    connect(m_pageSpin, qOverload<int>(&QSpinBox::valueChanged),
            this, [this](int value) {
        if (m_spinSync)
            return;  // scrollbar-tracking echo, not user input
        m_canvas->goToPage(value - 1);
        updatePageControls();
    });

    m_toolbar->addSeparator();
    QAction *zoomOut = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("zoom-out")),
        tr("Zoom out"));
    QAction *zoomIn = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("zoom-in")),
        tr("Zoom in"));
    connect(zoomOut, &QAction::triggered, this, [this]() {
        m_canvas->setZoom(m_canvas->zoom() / 1.25);
    });
    connect(zoomIn, &QAction::triggered, this, [this]() {
        m_canvas->setZoom(m_canvas->zoom() * 1.25);
    });
    m_zoomBox->setEditable(true);
    m_zoomBox->setInsertPolicy(QComboBox::NoInsert);
    const QStringList presets = {
        QStringLiteral("50%"), QStringLiteral("75%"),
        QStringLiteral("100%"), QStringLiteral("125%"),
        QStringLiteral("150%"), QStringLiteral("200%"),
        QStringLiteral("400%"),
    };
    m_zoomBox->addItems(presets);
    m_zoomBox->setCurrentText(QStringLiteral("100%"));
    m_toolbar->addWidget(m_zoomBox);
    connect(m_zoomBox, &QComboBox::activated, this,
            [this](int) {
        QString text = m_zoomBox->currentText();
        text.remove(QLatin1Char('%'));
        bool ok = false;
        const double pct = text.toDouble(&ok);
        if (ok && pct > 0)
            m_canvas->setZoom(pct / 100.0);
    });
    connect(m_canvas, &PdfCanvas::zoomChanged,
            this, &PdfView::syncZoomSpin);

    QAction *fit = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("zoom-fit-width")),
        tr("Fit page width"));
    fit->setCheckable(true);
    connect(fit, &QAction::toggled, this,
            [this](bool on) { m_canvas->setFitWidth(on); });
    connect(m_canvas, &PdfCanvas::zoomChanged, this,
            [this, fit]() {
        // A manual zoom drops fit-width; the fit-triggered zoom
        // change itself must not bounce the action back off.
        if (!m_canvas->fitWidth() && fit->isChecked())
            fit->setChecked(false);
    });

    m_toolbar->addSeparator();
    QAction *find = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("edit-find")),
        tr("Find in document"));
    find->setShortcut(QKeySequence::Find);
    connect(find, &QAction::triggered,
            this, &PdfView::showFindBar);
    QAction *reload = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("view-refresh")),
        tr("Reload document"));
    connect(reload, &QAction::triggered, this, [this]() {
        emit reloadRequested(m_sourceUrl);
    });
    QAction *print = m_toolbar->addAction(
        QIcon::fromTheme(QStringLiteral("document-print")),
        tr("Print document"));
    print->setShortcut(QKeySequence::Print);
    connect(print, &QAction::triggered,
            this, &PdfView::printDocument);

    // Scrollbar -> current page tracking.
    connect(m_canvas->verticalScrollBar(), &QScrollBar::valueChanged,
            this, &PdfView::updatePageControls);
    connect(m_canvas, &PdfCanvas::currentPageChanged,
            this, &PdfView::updatePageControls);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);
    layout->addWidget(m_toolbar);
    layout->addWidget(m_canvas, 1);
}

PdfView::~PdfView()
{
    delete m_client;
}

void PdfView::setDocument(const QString &localPath,
                          const QUrl &sourceUrl)
{
    m_localPath = localPath;
    m_sourceUrl = sourceUrl;
    m_canvas->clearDocument();
    delete m_client;
    m_client = new PdfDocumentClient(this);
    connect(m_client, &PdfDocumentClient::ready,
            this, &PdfView::handleClientReady);
    connect(m_client, &PdfDocumentClient::failed,
            this, [this](const QString &message) {
        emit documentFailed(message);
    });
    m_client->open(localPath);
}

void PdfView::clearDocument()
{
    m_canvas->clearDocument();
    delete m_client;
    m_client = nullptr;
    m_localPath.clear();
    m_sourceUrl.clear();
    m_pageSpin->setMaximum(1);
    m_pageSpin->setValue(1);
    m_pageCount->clear();
}

QString PdfView::documentTitle() const
{
    if (m_client && !m_client->title().isEmpty())
        return m_client->title();
    return QFileInfo(m_localPath).fileName();
}

bool PdfView::isReady() const
{
    return m_client && m_client->isReady();
}

QString PdfView::backendName() const
{
    return m_client ? m_client->backendName() : QString();
}

void PdfView::handleClientReady()
{
    m_canvas->setClient(m_client);
    m_pageSpin->setMaximum(qMax(1, m_canvas->pageCount()));
    m_pageCount->setText(
        tr("of %1").arg(m_canvas->pageCount()));
    updatePageControls();
    emit documentReady();
    emit titleChanged(documentTitle());
}

void PdfView::updatePageControls()
{
    const int page = m_canvas->currentPage();
    if (m_pageSpin->value() != page + 1) {
        m_spinSync = true;
        m_pageSpin->setValue(page + 1);
        m_spinSync = false;
    }
}

void PdfView::syncZoomSpin(double zoom)
{
    const QString text =
        QStringLiteral("%1%").arg(int(zoom * 100));
    if (m_zoomBox->currentText() != text)
        m_zoomBox->setCurrentText(text);
}

void PdfView::showFindBar()
{
    if (!m_findBar) {
        m_findBar = new QWidget(this);
        QHBoxLayout *row = new QHBoxLayout(m_findBar);
        row->setContentsMargins(4, 2, 4, 2);
        m_findEdit = new QLineEdit(m_findBar);
        m_findEdit->setPlaceholderText(tr("Find in document"));
        QPushButton *prev =
            new QPushButton(tr("Previous"), m_findBar);
        QPushButton *next =
            new QPushButton(tr("Next"), m_findBar);
        m_findStatus = new QLabel(m_findBar);
        QPushButton *close = new QPushButton(
            QIcon::fromTheme(QStringLiteral("window-close")),
            QString(), m_findBar);
        close->setFlat(true);
        row->addWidget(m_findEdit, 1);
        row->addWidget(prev);
        row->addWidget(next);
        row->addWidget(m_findStatus);
        row->addWidget(close);
        connect(m_findEdit, &QLineEdit::returnPressed,
                this, &PdfView::findNext);
        connect(m_findEdit, &QLineEdit::textChanged,
                this, [this](const QString &text) {
            if (!text.isEmpty())
                m_canvas->find(text, 1);
            else
                m_findStatus->clear();
        });
        connect(next, &QPushButton::clicked,
                this, &PdfView::findNext);
        connect(prev, &QPushButton::clicked,
                this, &PdfView::findPrevious);
        connect(close, &QPushButton::clicked,
                m_findBar, &QWidget::hide);
        connect(m_canvas, &PdfCanvas::findStatus,
                m_findStatus, &QLabel::setText);
        layout()->addWidget(m_findBar);
    }
    m_findBar->show();
    m_findEdit->setFocus();
    m_findEdit->selectAll();
}

void PdfView::findNext()
{
    if (m_findEdit)
        m_canvas->find(m_findEdit->text(), 1);
}

void PdfView::findPrevious()
{
    if (m_findEdit)
        m_canvas->find(m_findEdit->text(), -1);
}

void PdfView::printDocument()
{
    if (!m_client || !m_client->isReady()) {
        QMessageBox::information(this, tr("Print Document"),
                                 tr("The document is still loading."));
        return;
    }
    QPrinter printer(QPrinter::HighResolution);
    printer.setDocName(documentTitle());
    QPrintDialog dialog(&printer, this);
    dialog.setWindowTitle(tr("Print Document"));
    if (dialog.exec() != QDialog::Accepted)
        return;
    QString error;
    if (!m_canvas->printPages(&printer, &error))
        QMessageBox::warning(this, tr("Print Document"),
                             tr("Cannot print: %1").arg(error));
}

void PdfView::keyPressEvent(QKeyEvent *event)
{
    if (event->matches(QKeySequence::Find)) {
        showFindBar();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape && m_findBar
            && m_findBar->isVisible()) {
        m_findBar->hide();
        m_canvas->setFocus();
        event->accept();
        return;
    }
    QWidget::keyPressEvent(event);
}
