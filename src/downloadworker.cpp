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
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA  02110-1301  USA
 */

#include "downloadworker.h"

#include "rustdl.h"

#include <qfileinfo.h>
#include <qhash.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qstring.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

// No QObject, no event loop — the worker is a bare stdio protocol
// process so that the sandboxed surface stays as small as possible.
// Qt is used only for JSON + path handling (the crate's C ABI takes
// UTF-8 char* so QByteArray conversion is all that is needed).

namespace {

std::mutex g_outMutex;
std::mutex g_replyMutex;
std::condition_variable g_replyCv;
QHash<quint64, QJsonObject> g_gateReplies;
std::atomic<quint64> g_nextGateId{1};
std::atomic<bool> g_stdinEof{false};
std::atomic<bool> g_cancelRequested{false};
std::atomic<DlHandle> g_handle{0};

void emitLine(const QJsonObject &obj)
{
    const QByteArray data =
        QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard<std::mutex> lock(g_outMutex);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    std::fflush(stdout);
}

void copyOut(char *dst, size_t cap, const QByteArray &payload)
{
    if (!dst || cap == 0)
        return;
    const size_t n = cap - 1 < size_t(payload.size())
        ? cap - 1 : size_t(payload.size());
    std::memcpy(dst, payload.constData(), n);
    dst[n] = '\0';
}

const char *stateName(int32_t state)
{
    switch (state) {
    case DL_PROBING: return "probing";
    case DL_RUNNING: return "running";
    case DL_MERGING: return "merging";
    case DL_DONE: return "done";
    case DL_FAILED: return "failed";
    case DL_CANCELLED: return "cancelled";
    default: return "unknown";
    }
}

// The crate calls this from whichever worker thread performs the
// request — it blocks until the parent's reply arrives on stdin (or
// stdin dies, in which case the download fails closed).
int workerGate(const char *urlUtf8, const char *prevUtf8,
               const char *firstPartyUtf8, const char *scopeUtf8,
               char *outUrl, size_t outUrlCap,
               char *outReason, size_t outReasonCap, void *)
{
    // Serialise the whole request/reply round trip — gate calls are
    // rare (first request plus redirect hops) so one mutex for the
    // wait keeps the reply matching trivial.
    static std::mutex gateMutex;
    std::lock_guard<std::mutex> gateLock(gateMutex);

    const quint64 id = g_nextGateId.fetch_add(1);
    QJsonObject request;
    request.insert(QLatin1String("ev"), QLatin1String("gate"));
    request.insert(QLatin1String("id"), double(id));
    request.insert(QLatin1String("url"),
                   QString::fromUtf8(urlUtf8 ? urlUtf8 : ""));
    request.insert(QLatin1String("prev_url"),
                   QString::fromUtf8(prevUtf8 ? prevUtf8 : ""));
    request.insert(QLatin1String("first_party"),
                   QString::fromUtf8(firstPartyUtf8 ? firstPartyUtf8
                                                  : ""));
    request.insert(QLatin1String("scope"),
                   QString::fromUtf8(scopeUtf8 ? scopeUtf8 : ""));
    emitLine(request);

    int action = DL_GATE_BLOCK;
    QByteArray payload;
    {
        std::unique_lock<std::mutex> lk(g_replyMutex);
        const bool answered = g_replyCv.wait_for(
            lk, std::chrono::seconds(30), [id] {
                return g_gateReplies.contains(id) || g_stdinEof.load();
            });
        if (answered && !g_stdinEof.load()) {
            const QJsonObject reply = g_gateReplies.take(id);
            action = reply.value(QLatin1String("action"))
                         .toInt(DL_GATE_BLOCK);
            payload = (action == DL_GATE_REWRITE)
                ? reply.value(QLatin1String("url")).toString().toUtf8()
                : reply.value(QLatin1String("reason"))
                      .toString().toUtf8();
        }
    }
    if (action == DL_GATE_REWRITE)
        copyOut(outUrl, outUrlCap, payload);
    else
        copyOut(outReason, outReasonCap, payload);
    return action;
}

void readerLoop()
{
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty())
            continue;
        const QJsonObject obj = QJsonDocument::fromJson(
            QByteArray::fromStdString(line)).object();
        if (obj.value(QLatin1String("ev")).toString()
            == QLatin1String("gate-reply")) {
            const quint64 id =
                quint64(obj.value(QLatin1String("id")).toDouble());
            std::lock_guard<std::mutex> lk(g_replyMutex);
            g_gateReplies.insert(id, obj);
            g_replyCv.notify_all();
        } else if (obj.value(QLatin1String("cmd")).toString()
                   == QLatin1String("cancel")) {
            g_cancelRequested = true;
            const DlHandle handle = g_handle.load();
            if (handle)
                dl_cancel(handle);
        }
    }
    // stdin gone: the parent died or abandoned us — cancel and let
    // the poll loop surface a terminal state.
    g_stdinEof = true;
    {
        std::lock_guard<std::mutex> lk(g_replyMutex);
        g_replyCv.notify_all();
    }
    const DlHandle handle = g_handle.load();
    if (handle)
        dl_cancel(handle);
}

} // namespace

int downloadWorkerMain()
{
    // The job description arrives on the first stdin line.
    std::string jobLine;
    if (!std::getline(std::cin, jobLine)) {
        std::fprintf(stderr, "arora: download worker got no job\n");
        return 2;
    }
    const QJsonObject job = QJsonDocument::fromJson(
        QByteArray::fromStdString(jobLine)).object();
    if (job.value(QLatin1String("cmd")).toString()
        != QLatin1String("job")) {
        std::fprintf(stderr, "arora: download worker got bad job\n");
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

    const QByteArray url =
        job.value(QLatin1String("url")).toString().toUtf8();
    const QByteArray destDir =
        job.value(QLatin1String("dest_dir")).toString().toUtf8();
    const QByteArray suggested =
        job.value(QLatin1String("suggested_name")).toString().toUtf8();
    const QByteArray cookieFile =
        job.value(QLatin1String("cookie_file")).toString().toUtf8();
    const QByteArray workDir =
        job.value(QLatin1String("work_dir")).toString().toUtf8();
    const int connections =
        job.value(QLatin1String("connections")).toInt(4);
    // options is an object in the protocol but dl_start wants the
    // serialized form — compact it back to JSON text.
    const QByteArray options = QJsonDocument(
        job.value(QLatin1String("options")).toObject())
            .toJson(QJsonDocument::Compact);

    if (url.isEmpty() || destDir.isEmpty() || workDir.isEmpty()) {
        emitLine({{QLatin1String("ev"), QLatin1String("error")},
                  {QLatin1String("code"), int(DL_INVALID_ARGUMENT)},
                  {QLatin1String("message"),
                   QLatin1String("job missing url/dest_dir/work_dir")}});
        return 2;
    }
    if (dl_set_temp_dir(workDir.constData()) != DL_OK) {
        emitLine({{QLatin1String("ev"), QLatin1String("error")},
                  {QLatin1String("code"), int(DL_IO)},
                  {QLatin1String("message"),
                   QLatin1String("dl_set_temp_dir failed")}});
        return 2;
    }
    dl_set_gate(&workerGate, nullptr);

    // The reader owns stdin from here on: gate replies and cancel
    // commands.  It must be running before dl_start because the first
    // gate call can arrive synchronously inside dl_start.
    std::thread reader(readerLoop);
    reader.detach();

    DlHandle handle = 0;
    const DlStatus started = dl_start(
        url.constData(), destDir.constData(),
        suggested.isEmpty() ? nullptr : suggested.constData(),
        connections,
        cookieFile.isEmpty() ? nullptr : cookieFile.constData(),
        options.constData(), &handle);
    if (started != DL_OK || !handle) {
        emitLine({{QLatin1String("ev"), QLatin1String("error")},
                  {QLatin1String("code"), int(started)},
                  {QLatin1String("message"),
                   QLatin1String("dl_start failed")}});
        return 2;
    }
    g_handle = handle;
    if (g_cancelRequested.load())
        dl_cancel(handle);

    QString lastFileName;
    int exitCode = 2;
    for (;;) {
        DlProgress progress;
        std::memset(&progress, 0, sizeof(progress));
        if (dl_poll(handle, &progress) != DL_OK)
            break;

        char *name = dl_file_name(handle);
        if (name) {
            const QString current = QString::fromUtf8(name);
            dl_string_free(name);
            if (current != lastFileName) {
                lastFileName = current;
                QJsonObject out;
                out.insert(QLatin1String("ev"),
                           QLatin1String("file-name"));
                out.insert(QLatin1String("file_name"), current);
                emitLine(out);
            }
        }

        if (progress.state == DL_DONE
                || progress.state == DL_CANCELLED
                || progress.state == DL_FAILED) {
            QJsonObject out;
            out.insert(QLatin1String("bytes"),
                       double(progress.bytes_done));
            out.insert(QLatin1String("total"),
                       double(progress.bytes_total));
            if (progress.state == DL_DONE) {
                out.insert(QLatin1String("ev"), QLatin1String("done"));
                char *output = dl_output_path(handle);
                if (output) {
                    out.insert(QLatin1String("output"),
                               QString::fromUtf8(output));
                    dl_string_free(output);
                }
                exitCode = 0;
            } else if (progress.state == DL_CANCELLED) {
                out.insert(QLatin1String("ev"),
                           QLatin1String("cancelled"));
                exitCode = 3;
            } else {
                out.insert(QLatin1String("ev"), QLatin1String("error"));
                out.insert(QLatin1String("code"), int(DL_FAILED));
                char *msg = dl_error_message(handle);
                if (msg) {
                    out.insert(QLatin1String("message"),
                               QString::fromUtf8(msg));
                    dl_string_free(msg);
                }
            }
            emitLine(out);
            break;
        }

        QJsonObject out;
        out.insert(QLatin1String("ev"), QLatin1String("progress"));
        out.insert(QLatin1String("state"),
                   QLatin1String(stateName(progress.state)));
        out.insert(QLatin1String("bytes"), double(progress.bytes_done));
        out.insert(QLatin1String("total"),
                   double(progress.bytes_total));
        out.insert(QLatin1String("speed"), double(progress.speed_bps));
        emitLine(out);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    g_handle = 0;
    dl_free(handle);
    return exitCode;
}
