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

#ifndef DOWNLOADWORKER_H
#define DOWNLOADWORKER_H

// SAND02: entry point for `arora --download-worker` — the confined
// subprocess that runs the rustdl accelerated download engine.  The
// process never creates a QApplication, never touches QtWebEngine and
// never initialises the profile: it speaks a single-purpose JSONL
// protocol over stdin/stdout so it can run under a restrictive
// sandbox (bwrap on Linux) that exposes only the download's work dir
// and destination directory.
//
// Protocol — one JSON object per line, UTF-8:
//
//   parent -> worker (stdin), first line is always the job:
//     {"cmd":"job","url":..,"dest_dir":..,"suggested_name":..,
//      "connections":N,"cookie_file":..,"work_dir":..,
//      "options":{...dl options_json fields...},
//      "probe_paths":[...]}
//     {"cmd":"cancel"}
//     {"ev":"gate-reply","id":N,"action":0|1|2,"url"|"reason":..}
//
//   worker -> parent (stdout):
//     {"ev":"probe","path":..,"readable":bool}   (per probe_paths entry)
//     {"ev":"progress","state":"..","bytes":N,"total":N,"speed":N}
//     {"ev":"file-name","file_name":..}
//     {"ev":"gate","id":N,"url":..,"prev_url":..,"first_party":..,
//      "scope":..}
//     {"ev":"done","output":..,"bytes":N,"total":N}
//     {"ev":"cancelled"}
//     {"ev":"error","code":N,"message":..}
//
// The gate path exists because a bare worker process cannot consult
// AdBlockNetwork, HTTPS-only policy or the tor proxy rules — those
// live in the GUI process.  dl_set_gate callbacks are marshalled to
// the parent over this protocol and the reply is returned to the
// crate; a parent that dies mid-round-trip fails the download closed.

int downloadWorkerMain();

#endif // DOWNLOADWORKER_H
