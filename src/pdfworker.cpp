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

#include "pdfworker.h"

#include <qfile.h>
#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qstring.h>

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

#ifdef ARORA_RUSTCORE
#include "rustcore.h"
#endif

// No QObject, no event loop — the worker is a bare stdio protocol
// process so that the sandboxed surface stays as small as possible
// (pdfworker.h documents the JSONL contract).  One document lives per
// process; render results are written as PNG files into the job's
// work_dir rather than base64'd through the pipe.

namespace {

std::mutex g_outMutex;

void emitLine(const QJsonObject &obj)
{
    const QByteArray data =
        QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    std::fflush(stdout);
}

// Only the ARORA_RUSTCORE path reports errors; in the no-rust build
// this helper is intentionally idle.
[[maybe_unused]] void emitError(qint64 req, int code, const QString &message)
{
    QJsonObject out;
    out.insert(QLatin1String("ev"), QLatin1String("error"));
    if (req >= 0)
        out.insert(QLatin1String("req"), double(req));
    out.insert(QLatin1String("code"), code);
    out.insert(QLatin1String("message"), message);
    emitLine(out);
}

} // namespace

int pdfWorkerMain()
{
#ifdef ARORA_RUSTCORE
    // The job description arrives on the first stdin line.
    std::string jobLine;
    if (!std::getline(std::cin, jobLine)) {
        std::fprintf(stderr, "arora: pdf worker got no job\n");
        return 2;
    }
    const QJsonObject job = QJsonDocument::fromJson(
        QByteArray::fromStdString(jobLine)).object();
    if (job.value(QLatin1String("cmd")).toString()
        != QLatin1String("job")) {
        std::fprintf(stderr, "arora: pdf worker got bad job\n");
        return 2;
    }

    // Diagnostic/test hook: report whether each requested path is
    // visible inside this sandbox before any work starts.
    const QJsonArray probes =
        job.value(QLatin1String("probe_paths")).toArray();
    for (const QJsonValue &entry : probes) {
        const QString path = entry.toString();
        const QFileInfo info(path);
        QJsonObject out;
        out.insert(QLatin1String("ev"), QLatin1String("probe"));
        out.insert(QLatin1String("path"), path);
        out.insert(QLatin1String("readable"),
                   info.exists() && info.isReadable());
        emitLine(out);
    }

    const QString pdfPath = job.value(QLatin1String("pdf")).toString();
    const QString workDir =
        job.value(QLatin1String("work_dir")).toString();
    if (pdfPath.isEmpty() || workDir.isEmpty()) {
        emitError(-1, 1, QLatin1String("job missing pdf/work_dir"));
        return 2;
    }

    QByteArray bytes;
    {
        QFile in(pdfPath);
        if (!in.open(QIODevice::ReadOnly)) {
            emitError(-1, 2, QLatin1String("cannot read pdf: ")
                              + in.errorString());
            return 2;
        }
        bytes = in.readAll();
    }
    const uintptr_t doc = rc_pdfdoc_open(
        reinterpret_cast<const uint8_t *>(bytes.constData()),
        size_t(bytes.size()));
    bytes.clear();
    if (!doc) {
        char *msg = rc_last_error_message();
        emitError(-1, 7, QString::fromUtf8(msg ? msg : "pdf open failed"));
        rc_string_free(msg);
        return 3;
    }

    const int pages = rc_pdfdoc_page_count(doc);
    QJsonObject meta;
    meta.insert(QLatin1String("ev"), QLatin1String("meta"));
    meta.insert(QLatin1String("pages"), pages);
    if (char *title = rc_pdfdoc_title(doc)) {
        meta.insert(QLatin1String("title"), QString::fromUtf8(title));
        rc_string_free(title);
    }
    QJsonArray sizes;
    for (int i = 0; i < pages; ++i) {
        double w = 0.0, h = 0.0;
        if (rc_pdfdoc_page_size(doc, i, &w, &h) == RC_OK)
            sizes.append(QJsonArray{w, h});
        else
            sizes.append(QJsonArray{0.0, 0.0});
    }
    meta.insert(QLatin1String("sizes"), sizes);
    emitLine(meta);

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        const QJsonObject cmd = QJsonDocument::fromJson(
            QByteArray::fromStdString(line)).object();
        const QString op = cmd.value(QLatin1String("cmd")).toString();
        if (op == QLatin1String("stop"))
            break;

        const qint64 req = qint64(cmd.value(QLatin1String("req")).toDouble(-1));
        if (op == QLatin1String("render")) {
            const int page = cmd.value(QLatin1String("page")).toInt(-1);
            const double scale =
                cmd.value(QLatin1String("scale")).toDouble(1.0);
            RcBuffer png = { nullptr, 0 };
            const RcStatus status = rc_pdfdoc_render_png(
                doc, page, scale, &png);
            if (status != RC_OK || !png.data) {
                char *msg = rc_last_error_message();
                emitError(req, int(status),
                          QString::fromUtf8(msg ? msg : "render failed"));
                rc_string_free(msg);
                continue;
            }
            const QString name =
                QStringLiteral("r%1.png").arg(req);
            QFile out(workDir + QLatin1Char('/') + name);
            if (!out.open(QIODevice::WriteOnly)) {
                rc_buffer_free(png);
                emitError(req, 2, QLatin1String("cannot write png"));
                continue;
            }
            out.write(reinterpret_cast<const char *>(png.data),
                      qint64(png.len));
            out.close();
            // Cheap pixel dims for the parent — the PNG header is
            // ours so a fixed-offset read is safe.
            int w = 0, h = 0;
            if (png.len >= 24) {
                const uint8_t *d = png.data;
                w = (d[16] << 24) | (d[17] << 16) | (d[18] << 8) | d[19];
                h = (d[20] << 24) | (d[21] << 16) | (d[22] << 8) | d[23];
            }
            rc_buffer_free(png);
            QFile::setPermissions(out.fileName(),
                QFileDevice::ReadOwner | QFileDevice::WriteOwner);
            QJsonObject ev;
            ev.insert(QLatin1String("ev"), QLatin1String("page"));
            ev.insert(QLatin1String("req"), double(req));
            ev.insert(QLatin1String("file"), name);
            ev.insert(QLatin1String("w"), w);
            ev.insert(QLatin1String("h"), h);
            emitLine(ev);
        } else if (op == QLatin1String("find")) {
            const QByteArray needle =
                cmd.value(QLatin1String("needle")).toString().toUtf8();
            const int from = cmd.value(QLatin1String("from")).toInt();
            const int dir = cmd.value(QLatin1String("dir")).toInt(1);
            int32_t total = 0;
            const int32_t hit = rc_pdfdoc_find(
                doc, needle.constData(), from, dir, &total);
            QJsonObject ev;
            ev.insert(QLatin1String("ev"), QLatin1String("find"));
            ev.insert(QLatin1String("req"), double(req));
            ev.insert(QLatin1String("page"), hit);
            ev.insert(QLatin1String("total"), total);
            emitLine(ev);
        }
        // Unknown commands are ignored — the parent only sends the
        // documented set.
    }

    rc_pdfdoc_close(doc);
    return 0;
#else
    std::fprintf(stderr, "arora: pdf worker needs the rust core\n");
    return 2;
#endif
}
