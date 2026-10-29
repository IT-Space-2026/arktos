/*
 * Copyright 2026 Arktos Browser - IT-Space
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

#ifndef WORKERIO_H
#define WORKERIO_H

#include <cstdio>
#include <iostream>
#include <mutex>
#include <string>

#include <qfileinfo.h>
#include <qjsonarray.h>
#include <qjsondocument.h>
#include <qjsonobject.h>
#include <qjsonvalue.h>
#include <qstring.h>

// CA08: the JSONL control-channel the sandboxed worker processes
// (pdfworker, downloadworker) each carried privately — one job
// document on the first stdin line, replies as compact JSON objects
// on stdout.
namespace WorkerIO {

// One JSON object per line on stdout, mutex-serialized so worker
// threads never interleave a write.
inline void emitLine(const QJsonObject &obj)
{
    static std::mutex outMutex;
    const QByteArray data =
        QJsonDocument(obj).toJson(QJsonDocument::Compact) + '\n';
    std::lock_guard<std::mutex> lock(outMutex);
    std::fwrite(data.constData(), 1, size_t(data.size()), stdout);
    std::fflush(stdout);
}

// Reads + validates the job document — every worker opens with the
// same {"cmd":"job"} envelope.  Returns false (and prints the
// worker-tagged stderr line) on a missing or malformed job.
inline bool readJob(const char *workerName, QJsonObject *job)
{
    std::string jobLine;
    if (!std::getline(std::cin, jobLine)) {
        std::fprintf(stderr, "arktos: %s worker got no job\n",
                     workerName);
        return false;
    }
    *job = QJsonDocument::fromJson(
        QByteArray::fromStdString(jobLine)).object();
    if (job->value(QLatin1String("cmd")).toString()
        != QLatin1String("job")) {
        std::fprintf(stderr, "arktos: %s worker got bad job\n",
                     workerName);
        return false;
    }
    return true;
}

// Diagnostic/test hook: report whether each requested path is
// visible inside this sandbox before any work starts.
inline void emitPathProbes(const QJsonObject &job)
{
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
}

} // namespace WorkerIO

#endif // WORKERIO_H
