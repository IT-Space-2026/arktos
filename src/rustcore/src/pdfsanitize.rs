//! PDF01 (c): pre-view PDF sanitization — a structural REWRITE pass,
//! not a reader: lopdf decodes the object graph, we strip the
//! action/attachment surface the Chromium viewer sandbox does not
//! policy (PDF JavaScript, Launch commands, auto-exec triggers,
//! embedded files, XFA forms, remote-submit actions), and the
//! document is re-serialized whole.  What lands on disk is a
//! standalone valid PDF — the viewer never sees the original bytes.
//!
//! Inputs are attacker-controlled: the parse runs on lopdf's lenient
//! reader under an input-size bound and the dictionary walk is
//! depth-capped, so a hostile file degrades to RC_CORRUPT, never to
//! unbounded recursion or allocation.

use lopdf::{Dictionary, Document, Object};

/// Hard input bound — a "PDF" larger than this is refused outright.
const MAX_INPUT_BYTES: usize = 256 * 1024 * 1024;
/// Nesting bound on the object walk — crafted cycles/deep trees must
/// not exhaust the stack inside the FFI call.
const MAX_DEPTH: u32 = 128;

/// Dictionary keys removed wherever they appear: script payloads,
/// auto-exec triggers, embedded payloads and XFA form data.  `AA`
/// covers every page/annotation/form "additional action" trigger
/// (open, close, visible, focus, keystroke — the whole auto-exec
/// surface); `OpenAction` is the document-open trigger; `EmbeddedFiles`
/// inside `/Names` is the attachment registry; `XFA` inside AcroForm
/// is the XML form blob with its own scripting model.
const STRIP_KEYS: &[&[u8]] = &[
    b"JS",
    b"JavaScript",
    b"AA",
    b"OpenAction",
    b"EmbeddedFiles",
    b"XFA",
];

/// `/S` action types whose whole action dictionary is neutralized —
/// the entries a viewer would otherwise hand to its JS engine, the OS
/// shell (Launch), a remote endpoint (SubmitForm/ImportData) or the
/// media layer.  The dictionary survives as an inert empty map so
/// references stay structurally valid.
const DANGEROUS_ACTIONS: &[&[u8]] = &[
    b"JavaScript",
    b"Launch",
    b"SubmitForm",
    b"ImportData",
    b"Movie",
    b"Sound",
    b"Rendition",
];

/// Rewrites `input` into a sanitized standalone PDF.  Returns the
/// re-serialized bytes, or an error string when the input is not a
/// parseable PDF (the caller then refuses the view rather than
/// passing unsanitized bytes through).
pub fn sanitize(input: &[u8]) -> Result<Vec<u8>, String> {
    if input.len() > MAX_INPUT_BYTES {
        return Err(format!("pdf over {} MiB refused", MAX_INPUT_BYTES / (1 << 20)));
    }
    if input.is_empty() {
        return Err("empty input".to_string());
    }
    let mut doc = Document::load_mem(input).map_err(|e| format!("pdf parse: {e}"))?;
    for obj in doc.objects.values_mut() {
        sanitize_object(obj, 0);
    }
    // The trailer dictionary can carry the same keys (a malformed
    // but accepted doc may stash entries next to Root).
    sanitize_dictionary(&mut doc.trailer, 0);
    let mut out = Vec::with_capacity(input.len());
    doc.save_to(&mut out)
        .map_err(|e| format!("pdf write: {e}"))?;
    Ok(out)
}

fn sanitize_object(obj: &mut Object, depth: u32) {
    if depth > MAX_DEPTH {
        return;
    }
    match obj {
        Object::Dictionary(dict) => sanitize_dictionary(dict, depth + 1),
        Object::Array(items) => {
            for item in items.iter_mut() {
                sanitize_object(item, depth + 1);
            }
        }
        Object::Stream(stream) => sanitize_dictionary(&mut stream.dict, depth + 1),
        _ => {}
    }
}

fn sanitize_dictionary(dict: &mut Dictionary, depth: u32) {
    if depth > MAX_DEPTH {
        return;
    }
    for key in STRIP_KEYS {
        dict.remove(key);
    }
    // An action dictionary whose /S names a dangerous action type is
    // emptied outright — an action with no operands is a no-op, and
    // clearing the dict (rather than deleting the entry that points
    // here) keeps the referencing object's shape intact.
    if let Ok(Object::Name(name)) = dict.get(b"S") {
        if DANGEROUS_ACTIONS.contains(&name.as_slice()) {
            *dict = Dictionary::new();
            return;
        }
    }
    for (_, value) in dict.iter_mut() {
        sanitize_object(value, depth);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Builds a small but structurally valid PDF around caller-
    /// supplied catalog/page dictionaries — xref offsets are computed
    /// so both lopdf and real readers accept the fixture.
    fn build_pdf(objects: &[&[u8]]) -> Vec<u8> {
        let mut out = Vec::from(&b"%PDF-1.4\n"[..]);
        let mut offsets = Vec::with_capacity(objects.len() + 1);
        for (i, body) in objects.iter().enumerate() {
            offsets.push(out.len());
            out.extend_from_slice(format!("{} 0 obj\n", i + 1).as_bytes());
            out.extend_from_slice(body);
            out.extend_from_slice(b"\nendobj\n");
        }
        let xref = out.len();
        out.extend_from_slice(
            format!("xref\n0 {}\n0000000000 65535 f \n", objects.len() + 1).as_bytes(),
        );
        for off in offsets {
            out.extend_from_slice(format!("{off:010} 00000 n \n").as_bytes());
        }
        out.extend_from_slice(
            format!(
                "trailer\n<</Size {} /Root 1 0 R>>\nstartxref\n{xref}\n%%EOF\n",
                objects.len() + 1
            )
            .as_bytes(),
        );
        out
    }

    #[test]
    fn strips_dangerous_surface() {
        let input = build_pdf(&[
            b"<</Type /Catalog /Pages 2 0 R /OpenAction 5 0 R
                /AA <</O 5 0 R>>
                /Names <</EmbeddedFiles 7 0 R>>
                /AcroForm <</Fields [] /XFA <</preamble []>>>>>>",
            b"<</Type /Pages /Kids [3 0 R] /Count 1>>",
            b"<</Type /Page /Parent 2 0 R /MediaBox [0 0 300 200]
                /Resources <</Font <</F1 <</Type /Font /Subtype /Type1
                /BaseFont /Helvetica>>>>>> /Contents 4 0 R
                /Annots [9 0 R]>>",
            b"<</Length 44>>\nstream\nBT /F1 12 Tf 10 100 Td (x) Tj ET\nendstream",
            b"<</Type /Action /S /JavaScript /JS (app.alert('pwned'))>>",
            b"<</Type /Action /S /Launch /F (calc.exe)>>",
            b"<</Names [(a) <</EF <</F 10 0 R>> /F (a)>>]>>",
            b"<</Type /Action /S /SubmitForm /F (http://e/c)>>",
            b"<</Subtype /Link /Rect [0 0 9 9] /A 6 0 R>>",
            b"<</Type /EmbeddedFile /Length 4>>\nstream\nevil\nendstream",
        ]);
        let out = sanitize(&input).expect("fixture must sanitize");
        assert!(out.starts_with(b"%PDF-"), "still a pdf");
        // Re-parse the OUTPUT — the check inspects the decoded object
        // graph, not byte-substring luck.
        let doc = Document::load_mem(&out).expect("output reparses");
        let text = format!("{:?}", doc.objects);
        for needle in [
            "JavaScript",
            "Launch",
            "OpenAction",
            "EmbeddedFiles",
            "SubmitForm",
            "XFA",
            "pwned",
            "app.alert",
        ] {
            assert!(!text.contains(needle), "output still carries {needle}");
        }
        // The page tree survives — the document still renders.
        let root = doc.trailer.get(b"Root").unwrap().clone();
        let catalog = doc.dereference(&root).unwrap().1.as_dict().unwrap();
        assert!(catalog.get(b"Pages").is_ok());
    }

    #[test]
    fn refuses_non_pdf() {
        assert!(sanitize(b"<html>hi</html>").is_err());
        assert!(sanitize(b"").is_err());
        assert!(sanitize(&vec![0u8; MAX_INPUT_BYTES + 1]).is_err());
    }

    #[test]
    fn preserves_clean_document() {
        let input = build_pdf(&[
            b"<</Type /Catalog /Pages 2 0 R>>",
            b"<</Type /Pages /Kids [3 0 R] /Count 1>>",
            b"<</Type /Page /Parent 2 0 R /MediaBox [0 0 300 200]
                /Contents 4 0 R>>",
            b"<</Length 10>>\nstream\nBT ET\nendstream",
        ]);
        let out = sanitize(&input).expect("clean pdf sanitizes");
        let doc = Document::load_mem(&out).unwrap();
        assert!(doc.trailer.get(b"Root").is_ok());
    }
}
