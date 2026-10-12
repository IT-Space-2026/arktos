//! PDF02: page rasterization for the shell-owned document view.
//!
//! The renderer is `hayro` — a pure-Rust rasterizer whose whole
//! dependency tree forbids unsafe code — running inside the
//! `arora --pdf-worker` confined subprocess (bwrap allowlist: the
//! staged PDF read-only at a fixed /arora-rt path, a per-view work
//! dir as the only writable mount, no network, no $HOME).  Every
//! PDF reaching this module has already been through the
//! `pdfsanitize` rewrite, so the bytes are structurally clean; the
//! sandbox still bounds whatever a renderer bug could reach.
//!
//! One document per worker process.  The handle is a raw pointer to
//! a heap-owned [`PdfDocument`] — the parent opens once, issues
//! render/find calls, and closes; there is no concurrent access.
//!
//! Text for find-in-document is extracted lazily per page through
//! `lopdf` (the interpreter exposes glyph draws, not search-ready
//! text), so `find` degrades to page-granularity matches — the view
//! jumps to the matching page rather than painting hit rects.

use hayro::hayro_interpret::InterpreterSettings;
use hayro::hayro_syntax::Pdf;
use hayro::vello_cpu::color::palette::css::WHITE;
use hayro::{PixmapSettings, RenderCache, RenderSettings};

pub struct PdfDocument {
    pdf: Pdf,
    /// Second parse kept for text extraction — hayro's interpreter
    /// reports glyph draws, not searchable text runs.
    text_doc: Option<lopdf::Document>,
    /// Extracted page text, page index -> decoded content.  Pages are
    /// fetched on first `find` hit and memoised.
    page_text: Vec<Option<String>>,
    settings: InterpreterSettings,
}

impl PdfDocument {
    pub fn open(bytes: Vec<u8>) -> Result<Self, String> {
        let pdf = Pdf::new(bytes.clone()).map_err(|e| format!("pdf open: {e:?}"))?;
        let text_doc = lopdf::Document::load_mem(&bytes).ok();
        let page_text = vec![None; pdf.pages().iter().count()];
        Ok(Self {
            pdf,
            text_doc,
            page_text,
            settings: InterpreterSettings::default(),
        })
    }

    pub fn page_count(&self) -> usize {
        self.pdf.pages().iter().count()
    }

    /// Page dimensions in PDF points (1/72 inch), pre-zoom.
    pub fn page_size(&self, page: usize) -> Option<(f64, f64)> {
        let p = self.pdf.pages().get(page)?;
        let (w, h) = p.render_dimensions();
        Some((w as f64, h as f64))
    }

    /// Render `page` at `scale` (1.0 = CSS-point resolution) and
    /// return PNG-encoded bytes.
    pub fn render_png(&mut self, page: usize, scale: f64) -> Result<Vec<u8>, String> {
        let p = self
            .pdf
            .pages()
            .get(page)
            .ok_or_else(|| format!("page {page} out of range"))?;
        // Absurd bitmap requests are a memory DoS — a 4k page at
        // 10x zoom is ~130 MP.  Cap the pixel budget, not the zoom
        // itself (the view clamps zoom anyway).
        let (w, h) = self.page_size(page).unwrap_or((0.0, 0.0));
        let pixels = w * scale * h * scale;
        if pixels > 48.0 * 1024.0 * 1024.0 {
            return Err(format!("render {w}x{h} at {scale}x exceeds pixel cap"));
        }
        // The cache borrows the page's lifetime (hayro makes it
        // invariant), so it can't live on the struct — renders here
        // are one-shot per worker request anyway.
        let cache = RenderCache::new();
        let pixmap = hayro::render(
            p,
            &cache,
            &self.settings,
            &RenderSettings::default(),
            &PixmapSettings {
                x_scale: scale as f32,
                y_scale: scale as f32,
                bg_color: WHITE,
            },
        );
        pixmap
            .into_png()
            .map_err(|e| format!("png encode: {e}"))
    }

    /// Document info /Title, best-effort (lopdf string decode).
    pub fn title(&self) -> Option<String> {
        let doc = self.text_doc.as_ref()?;
        let info = doc.trailer.get(b"Info").ok()?;
        let dict = doc.dereference(info).ok()?.1.as_dict().ok()?;
        let title = dict.get(b"Title").ok()?;
        let bytes = match title {
            lopdf::Object::String(s, _) => s.clone(),
            other => doc.dereference(other).ok()?.1.as_str().ok()?.to_vec(),
        };
        let text = String::from_utf8_lossy(&bytes).trim().to_string();
        if text.is_empty() {
            None
        } else {
            Some(text)
        }
    }

    fn text_for_page(&mut self, page: usize) -> String {
        if let Some(cached) = &self.page_text[page] {
            return cached.clone();
        }
        // lopdf page numbers are 1-based.
        let text = self
            .text_doc
            .as_ref()
            .and_then(|d| d.extract_text(&[(page + 1) as u32]).ok())
            .unwrap_or_default();
        self.page_text[page] = Some(text.clone());
        text
    }

    /// Count every (case-insensitive) occurrence across the document.
    /// Used for the find bar's match total.
    pub fn count_occurrences(&mut self, needle: &str) -> usize {
        let folded = needle.to_lowercase();
        if folded.is_empty() {
            return 0;
        }
        let mut total = 0;
        for page in 0..self.page_count() {
            let hay = self.text_for_page(page).to_lowercase();
            total += hay.matches(&folded).count();
        }
        total
    }

    /// Find the page containing the next/prev occurrence after
    /// `from_page` (inclusive of from_page on dir=+1, exclusive on
    /// wrap).  Returns the page index, scanning every page at most
    /// once — a wrap reports the first hit in scan order.
    pub fn find(&mut self, needle: &str, from_page: i64, dir: i64) -> Option<usize> {
        let folded = needle.to_lowercase();
        let count = self.page_count() as i64;
        if folded.is_empty() || count == 0 {
            return None;
        }
        let step = if dir < 0 { -1 } else { 1 };
        let mut page = from_page.rem_euclid(count);
        for _ in 0..count {
            if self.text_for_page(page as usize).to_lowercase().contains(&folded) {
                return Some(page as usize);
            }
            page = (page + step).rem_euclid(count);
        }
        None
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Minimal two-page PDF whose text extracts through lopdf —
    /// Helvetica is a base-14 font so hayro's embed-fonts path covers
    /// the render side too.
    fn fixture() -> Vec<u8> {
        let stream1 = "BT /F1 24 Tf 50 150 Td (hello world) Tj ET";
        let stream2 = "BT /F1 24 Tf 50 150 Td (goodbye) Tj ET";
        let objs: Vec<String> = vec![
            "<< /Type /Catalog /Pages 2 0 R >>".into(),
            "<< /Type /Pages /Kids [3 0 R 4 0 R] /Count 2 >>".into(),
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200]
                /Resources << /Font << /F1 5 0 R >> >> /Contents 6 0 R >>"
                .into(),
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 300 200]
                /Resources << /Font << /F1 5 0 R >> >> /Contents 7 0 R >>"
                .into(),
            "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>".into(),
            format!("<< /Length {} >>\nstream\n{}\nendstream", stream1.len(), stream1),
            format!("<< /Length {} >>\nstream\n{}\nendstream", stream2.len(), stream2),
        ];
        let mut out = Vec::from(&b"%PDF-1.4\n"[..]);
        let mut offsets = Vec::new();
        for (i, body) in objs.iter().enumerate() {
            offsets.push(out.len());
            out.extend_from_slice(format!("{} 0 obj\n", i + 1).as_bytes());
            out.extend_from_slice(body.as_bytes());
            out.extend_from_slice(b"\nendobj\n");
        }
        let xref = out.len();
        out.extend_from_slice(
            format!("xref\n0 {}\n0000000000 65535 f \n", objs.len() + 1).as_bytes(),
        );
        for off in offsets {
            out.extend_from_slice(format!("{off:010} 00000 n \n").as_bytes());
        }
        out.extend_from_slice(
            format!(
                "trailer\n<</Size {} /Root 1 0 R>>\nstartxref\n{xref}\n%%EOF\n",
                objs.len() + 1
            )
            .as_bytes(),
        );
        out
    }

    #[test]
    fn opens_and_reports_geometry() {
        let doc = PdfDocument::open(fixture()).unwrap();
        assert_eq!(doc.page_count(), 2);
        assert_eq!(doc.page_size(0), Some((300.0, 200.0)));
        assert!(doc.page_size(9).is_none());
    }

    #[test]
    fn renders_page_to_png() {
        let mut doc = PdfDocument::open(fixture()).unwrap();
        let png = doc.render_png(0, 2.0).unwrap();
        assert!(png.starts_with(&[0x89, b'P', b'N', b'G']));
        // 300x200 pt at 2x -> a non-trivial raster.
        assert!(png.len() > 200);
        assert!(doc.render_png(7, 1.0).is_err());
        // Pixel cap refuses DoS-scale requests.
        assert!(doc.render_png(0, 40.0).is_err());
    }

    #[test]
    fn finds_text_by_page() {
        let mut doc = PdfDocument::open(fixture()).unwrap();
        assert_eq!(doc.find("goodbye", 0, 1), Some(1));
        assert_eq!(doc.find("hello", 1, -1), Some(0));
        assert_eq!(doc.find("absent", 0, 1), None);
        assert_eq!(doc.count_occurrences("hello"), 1);
        assert_eq!(doc.count_occurrences("o"), 4);
    }

    #[test]
    fn rejects_garbage() {
        assert!(PdfDocument::open(b"not a pdf".to_vec()).is_err());
        assert!(PdfDocument::open(Vec::new()).is_err());
    }
}
