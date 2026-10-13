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

#ifndef READERSANITIZER_H
#define READERSANITIZER_H

#include <qstring.h>

// READ05: app-side sanitizer for reader-mode article fragments.
//
// The reader pipeline (ReaderMode + src/data/reader.js) extracts an
// article's HTML in an isolated script world, but anything rendered
// back into a document crosses this boundary first: the fragment is
// re-tokenized in C++ where page JavaScript cannot tamper with the
// policy, filtered down to a safe element/attribute allowlist and
// re-serialized, then displayed inside a sandboxed iframe as the
// second line of defence.  The policy ports what reader.js's
// DOMParser-based sanitize() did — interactive and navigation-
// capable nodes are removed, transparent wrappers keep their
// children, event handlers and javascript: URLs are scrubbed —
// plus a little tightening (data: links, style elements, srcdoc,
// ping) the sandbox makes cheap to take.
namespace ReaderSanitizer {

struct Result {
    // Re-serialized safe fragment — ready to embed as markup.
    QString html;
    // Decoded, trimmed text length — the input for the thin-content
    // floor (READ03's isReadableBody, moved C++-side).
    int textLength = 0;
    // A kept img/video/audio/svg/table/figure survived — media
    // carries its own weight on the thin-content floor.
    bool hasMedia = false;
};

Result sanitize(const QString &fragment);

} // namespace ReaderSanitizer

#endif // READERSANITIZER_H
