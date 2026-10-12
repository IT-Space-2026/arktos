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

#ifndef PDFWORKER_H
#define PDFWORKER_H

// PDF02: entry point for `arora --pdf-worker` — the confined
// subprocess that rasterizes PDF pages for the shell-owned document
// view.  The process never creates a QApplication, never touches
// QtWebEngine and never opens a profile: it speaks a single-purpose
// JSONL protocol over stdin/stdout so it can run under a restrictive
// bwrap that mounts only the staged PDF (read-only) and a work dir
// (rendered PNGs), with NO network namespace at all.
//
// Protocol — one JSON object per line, UTF-8:
//
//   parent -> worker (stdin), first line is always the job:
//     {"cmd":"job","pdf":..,"work_dir":..,"probe_paths":[...]}
//     {"cmd":"render","req":N,"page":P,"scale":S}
//     {"cmd":"find","req":N,"needle":..,"from":P,"dir":1|-1}
//     {"cmd":"stop"}
//
//   worker -> parent (stdout):
//     {"ev":"probe","path":..,"readable":bool}   (per probe_paths entry)
//     {"ev":"meta","pages":N,"title":..,"sizes":[[w,h]...]}
//     {"ev":"page","req":N,"file":"rN.png","w":px,"h":px}
//     {"ev":"find","req":N,"page":P,"total":M}   (page=-1: no match)
//     {"ev":"error","req":N?,"code":N,"message":..}
//
// Rendered pages land in <work_dir>/r<req>.png — the parent reads
// and unlinks them; work_dir is the only writable mount inside the
// sandbox.

int pdfWorkerMain();

#endif // PDFWORKER_H
