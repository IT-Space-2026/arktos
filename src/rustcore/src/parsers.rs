//! SEC19: memory-safe parsers for attacker-influenced document
//! formats — the audit outcome lives at the bottom of this comment.
//!
//! The Qt shell keeps its object interfaces 1:1; what changes is
//! WHERE untrusted bytes get parsed.  These entry points consume the
//! raw attacker-controlled document and hand back a small JSON
//! document the C++ side maps onto its existing types — Qt's XML/JSON
//! parsers only ever see output this crate produced.
//!
//!   * rc_opensearch_parse — OpenSearch 1.1 search-engine
//!     descriptors, fetched from arbitrary sites by
//!     OpenSearchManager.  Byte-for-byte port of OpenSearchReader's
//!     semantics (DTD rejection, namespace check, first-wins Url
//!     slots, flat Param/Parameter capture, early exit).  Result JSON:
//!     {"name","description","imageUrl",
//!      "search"|"suggestions"|"image":
//!          {"template","method","params":[["name","value"],...]}}
//!     A slot key is absent when no matching <Url> was accepted.
//!
//!   * rc_updatemanifest_parse — extension self-update "gupdate"
//!     manifests, remote XML fetched from each extension's update_url.
//!     Result JSON: {"offers":[[appid,status,codebase,version],...]} —
//!     the Qt side keeps every policy decision (multi-app matching,
//!     status/version handling, codebase scheme check).
//!
//!   * rc_suggest_parse — OpenSearch suggestions replies
//!     (["term", ["s1", ...]] remote JSON).  Result JSON is a plain
//!     array of strings; non-string entries become "" exactly like
//!     QJsonValue::toString() did.  RC_CORRUPT on malformed input so
//!     the caller can keep its "no signal on garbage" behaviour.
//!
//!   * rc_xbel_check — structural gate for XBEL bookmark documents.
//!     The full bookmark store belongs to RCORE02's migration (the
//!     BookmarkNode tree would marshal an entire recursive document
//!     model across the ABI for no gain), so this is the documented
//!     "sanitize in Rust, Qt parses validated bytes" middle ground:
//!     a document is well-formed, rooted at <xbel> (absent or "1.0"
//!     version) and at most 256 elements deep — the same contract
//!     XbelReader already enforced, now proven out before Qt XML sees
//!     a byte.  DOCTYPE is tolerated: the XBEL spec ships one and old
//!     KDE exports carry it (Qt never resolves the external subset;
//!     undeclared general entities still fail downstream as before).
//!
//! Encoding: quick-xml's encoding_rs decoder honours BOMs and the
//! <?xml encoding="..."?> declaration, matching what QXmlStreamReader
//! accepted on the same bytes.
//!
//! ## Audit notes (the rest of the attack surface)
//!   * bookmarks.xbel load + import and HTML->XBEL import conversion
//!     funnel through XbelReader::read(QIODevice*) — all gated above.
//!   * Session restore uses QDataStream over self-written blobs and
//!     is owned by the RCORE03 task — out of scope here.
//!   * extension manifest.json is local user-installed content, not
//!     remote input — left in Qt (bounded 1 MiB, QJsonDocument).
//!   * Adblock rules already parse inside the adblock-rust crate;
//!     remote feed bodies (blocklist/urlstrip updates) are parsed by
//!     the matching rustcore modules.

use quick_xml::events::Event;
use quick_xml::{Reader, XmlVersion};
use serde_json::{json, Map, Value};

use crate::error::{Fail, RcResult, RcStatus};

/// Mirrors OpenSearchReader's MaximumDocumentSize.
const MAX_OPENSEARCH: usize = 1024 * 1024;
/// Mirrors ExtensionManager's kUpdateManifestMaxBytes.
const MAX_UPDATE_MANIFEST: usize = 1024 * 1024;
/// Mirrors XbelReader's MaximumInputSize.
const MAX_XBEL: usize = 64 * 1024 * 1024;
/// Mirrors OpenSearchEngine's MaximumSuggestionsSize.
const MAX_SUGGESTIONS: usize = 256 * 1024;
/// Mirrors XbelReader's MaximumNestingDepth.
const XBEL_MAX_DEPTH: usize = 256;

const OPENSEARCH_NS: &str = "http://a9.com/-/spec/opensearch/1.1/";

fn corrupt<T>(msg: impl Into<String>) -> RcResult<T> {
    Err(Fail {
        status: RcStatus::Corrupt,
        msg: msg.into(),
    })
}

// ------------------------------------------------------------------
// event plumbing — a small owned-token layer over quick-xml so the
// grammar loops read like the QXmlStreamReader code they replace.
// expand_empty_elements makes <foo/> arrive as Start+End, exactly the
// token pair Qt produces, so the transliterations stay literal.
// ------------------------------------------------------------------

pub(crate) enum Ev {
    Start {
        /// Qualified name (prefix:local).
        name: String,
        /// (qualified name, normalized+unescaped value) in doc order.
        attrs: Vec<(String, String)>,
    },
    End {
        name: String,
    },
    Text(String),
    /// A general entity reference ("amp", "#38") the reader surfaced
    /// instead of resolving — DTDs are rejected, so only the five
    /// predefined names and character refs can ever resolve.
    Ref(String),
    DocType,
    Eof,
    Other,
}

pub(crate) struct Scan<'a> {
    reader: Reader<&'a [u8]>,
    buf: Vec<u8>,
    /// Open-element depth — quick-xml does not flag unclosed elements
    /// at Eof, so this backs the premature-end check Qt performs.
    pub depth: usize,
}

/// Qt auto-detects UTF-16 via BOM; quick-xml's `encoding` support
/// covers the single-byte encodings only, so BOM'd input is
/// transcoded up front (a UTF-16 document without a BOM is invalid
/// XML anyway — the parser reports it downstream).
pub(crate) fn bom_transcode(data: &[u8]) -> RcResult<std::borrow::Cow<'_, [u8]>> {
    use std::borrow::Cow;
    let (units, big_endian) = if data.starts_with(&[0xFF, 0xFE]) {
        (&data[2..], false)
    } else if data.starts_with(&[0xFE, 0xFF]) {
        (&data[2..], true)
    } else {
        return Ok(Cow::Borrowed(data));
    };
    if units.len() % 2 != 0 {
        return corrupt("truncated UTF-16 document");
    }
    let u16s: Vec<u16> = units
        .chunks_exact(2)
        .map(|c| {
            if big_endian {
                u16::from_be_bytes([c[0], c[1]])
            } else {
                u16::from_le_bytes([c[0], c[1]])
            }
        })
        .collect();
    let text = String::from_utf16(&u16s).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "invalid UTF-16 document".into(),
    })?;
    Ok(Cow::Owned(text.into_bytes()))
}

impl<'a> Scan<'a> {
    pub(crate) fn new(data: &'a [u8]) -> Scan<'a> {
        let mut reader = Reader::from_reader(data);
        reader.config_mut().expand_empty_elements = true;
        Scan {
            reader,
            buf: Vec::new(),
            depth: 0,
        }
    }

    pub(crate) fn next(&mut self) -> RcResult<Ev> {
        self.buf.clear();
        match self.reader.read_event_into(&mut self.buf) {
            Ok(Event::Start(e)) => {
                let mut attrs = Vec::with_capacity(4);
                for attr in e.attributes() {
                    let attr = attr.map_err(|e| Fail {
                        status: RcStatus::Corrupt,
                        msg: format!("bad attribute: {e}"),
                    })?;
                    // normalized_value applies the XML attribute-value
                    // normalization Qt performs, unescaping predefined
                    // entities + character refs and erroring on the
                    // rest (unresolvable without a DTD).
                    let value = attr
                        .normalized_value(XmlVersion::Implicit1_0)
                        .map_err(|e| Fail {
                            status: RcStatus::Corrupt,
                            msg: format!("bad attribute value: {e}"),
                        })?;
                    attrs.push((attr.key.as_ref().to_owned(), value.into_owned()));
                }
                Ok(Ev::Start {
                    name: e.name().as_ref().to_owned(),
                    attrs,
                })
            }
            Ok(Event::End(e)) => Ok(Ev::End {
                name: e.name().as_ref().to_owned(),
            }),
            Ok(Event::Text(e)) => {
                // EOL normalization only — entity references arrive as
                // their own Ref events.
                Ok(Ev::Text(
                    e.xml_content(XmlVersion::Implicit1_0).into_owned(),
                ))
            }
            Ok(Event::CData(e)) => Ok(Ev::Text(
                e.xml_content(XmlVersion::Implicit1_0).into_owned(),
            )),
            Ok(Event::GeneralRef(e)) => Ok(Ev::Ref(e.as_ref().to_owned())),
            Ok(Event::DocType(_)) => Ok(Ev::DocType),
            Ok(Event::Eof) => {
                if self.depth > 0 {
                    return corrupt("premature end of document");
                }
                Ok(Ev::Eof)
            }
            Ok(_) => Ok(Ev::Other),
            Err(e) => corrupt(format!("malformed XML: {e}")),
        }
        .map(|ev| {
            match ev {
                Ev::Start { .. } => self.depth += 1,
                Ev::End { .. } => self.depth = self.depth.saturating_sub(1),
                _ => {}
            }
            ev
        })
    }
}

/// The local part of a qualified name — QXmlStreamReader::name()
/// semantics (prefix stripped while namespace processing is on).
pub(crate) fn local(name: &str) -> &str {
    match name.rfind(':') {
        Some(i) => &name[i + 1..],
        None => name,
    }
}

/// First attribute whose LOCAL name matches — QXmlStreamAttributes::
/// value() semantics.  Present-but-empty counts as present.
pub(crate) fn attr<'a>(attrs: &'a [(String, String)], want: &str) -> Option<&'a str> {
    attrs
        .iter()
        .find(|(k, _)| local(k) == want)
        .map(|(_, v)| v.as_str())
}

pub(crate) fn attr_or_empty<'a>(attrs: &'a [(String, String)], want: &str) -> &'a str {
    attr(attrs, want).unwrap_or("")
}

/// Resolves the entity references legal in a DTD-less document — the
/// five predefined names plus decimal/hex character refs, matching
/// what QXmlStreamReader resolves itself.
pub(crate) fn resolve_entity(name: &str) -> Option<String> {
    match name {
        "amp" => Some("&".into()),
        "lt" => Some("<".into()),
        "gt" => Some(">".into()),
        "quot" => Some("\"".into()),
        "apos" => Some("'".into()),
        _ => {
            let digits = name.strip_prefix('#')?;
            let cp = match digits.strip_prefix('x').or_else(|| digits.strip_prefix('X'))
            {
                Some(hex) => u32::from_str_radix(hex, 16).ok()?,
                None => digits.parse().ok()?,
            };
            char::from_u32(cp).map(|c| c.to_string())
        }
    }
}

/// readElementText(): character data until the element's End.  A
/// nested start tag is an unexpected-element error, same as Qt.
pub(crate) fn read_element_text(scan: &mut Scan) -> RcResult<String> {
    let mut out = String::new();
    loop {
        match scan.next()? {
            Ev::Text(t) => out.push_str(&t),
            Ev::Ref(name) => {
                let resolved = resolve_entity(&name).ok_or_else(|| Fail {
                    status: RcStatus::Corrupt,
                    msg: format!("unresolved entity &{name};"),
                })?;
                out.push_str(&resolved);
            }
            Ev::End { .. } => return Ok(out),
            Ev::Start { .. } => return corrupt("unexpected nested element in text"),
            Ev::DocType => return corrupt("document must not contain a DTD"),
            Ev::Eof => return corrupt("premature end of document"),
            Ev::Other => {}
        }
    }
}

// ------------------------------------------------------------------
// OpenSearch 1.1 description documents
// ------------------------------------------------------------------

#[derive(Default)]
struct UrlSlot {
    template: String,
    method: String,
    params: Vec<(String, String)>,
}

#[derive(Default)]
struct Description {
    name: String,
    description: String,
    image_url: String,
    have_name: bool,
    have_description: bool,
    have_image_url: bool,
    search: Option<UrlSlot>,
    suggestions: Option<UrlSlot>,
    image: Option<UrlSlot>,
}

impl Description {
    /// The C++ reader's early-exit condition, verbatim.
    fn complete(&self) -> bool {
        self.have_name
            && self.have_description
            && self.suggestions.is_some()
            && self.search.is_some()
            && self.image.is_some()
            && self.have_image_url
    }
}

/// Resolves a root element's namespace URI from the xmlns
/// declarations it carries — the minimum of namespace processing the
/// root check needs (children match on local name only, like the C++
/// code did).
fn resolve_ns(name: &str, attrs: &[(String, String)]) -> String {
    let prefix = name.split(':').next().filter(|_| name.contains(':'));
    let want = match prefix {
        None => "xmlns".to_owned(),
        Some(p) => format!("xmlns:{p}"),
    };
    attrs
        .iter()
        .find(|(k, _)| k == &want)
        .map(|(_, v)| v.clone())
        .unwrap_or_default()
}

/// Captures Param/Parameter elements until the matching </Url> —
/// literal transliteration of the C++ inner loop, including its two
/// quirks: a Param nested anywhere inside <Url> is collected (flat
/// scan), and the first </Url> — even a nested one's — ends the scan.
fn scan_url_params(scan: &mut Scan) -> RcResult<Vec<(String, String)>> {
    let mut params = Vec::new();
    loop {
        match scan.next()? {
            Ev::End { name } if local(&name) == "Url" => return Ok(params),
            Ev::Eof => return corrupt("premature end of document inside <Url>"),
            Ev::DocType => return corrupt("document must not contain a DTD"),
            Ev::Start { name, attrs }
                if local(&name) == "Param" || local(&name) == "Parameter" =>
            {
                let key = attr_or_empty(&attrs, "name");
                let value = attr_or_empty(&attrs, "value");
                if !key.is_empty() && !value.is_empty() {
                    params.push((key.to_owned(), value.to_owned()));
                }
                // Consume up to this element's End without inspecting —
                // Param content is never itself parsed.
                loop {
                    match scan.next()? {
                        Ev::End { .. } => break,
                        Ev::Eof => {
                            return corrupt("premature end of document inside <Url>")
                        }
                        Ev::DocType => {
                            return corrupt("document must not contain a DTD")
                        }
                        _ => {}
                    }
                }
            }
            _ => {}
        }
    }
}

fn parse_opensearch(data: &[u8]) -> RcResult<Description> {
    if data.len() > MAX_OPENSEARCH {
        return corrupt("the OpenSearch description is too large");
    }
    let data = bom_transcode(data)?;
    let mut scan = Scan::new(&data);
    let mut desc = Description::default();

    // Prolog: the first start element must be the namespaced root; a
    // DTD anywhere is rejected (no legitimate descriptor carries one).
    loop {
        match scan.next()? {
            Ev::DocType => {
                return corrupt("the OpenSearch description must not contain a DTD")
            }
            Ev::Eof => return corrupt("the file is not an OpenSearch 1.1 file"),
            Ev::Start { name, attrs } => {
                if local(&name) != "OpenSearchDescription"
                    || resolve_ns(&name, &attrs) != OPENSEARCH_NS
                {
                    return corrupt("the file is not an OpenSearch 1.1 file");
                }
                break;
            }
            _ => {}
        }
    }

    loop {
        match scan.next()? {
            Ev::Eof => break,
            Ev::DocType => return corrupt("document must not contain a DTD"),
            Ev::Start { name, attrs } => {
                match local(&name) {
                    "ShortName" => {
                        desc.name = read_element_text(&mut scan)?;
                        desc.have_name = true;
                    }
                    "Description" => {
                        desc.description = read_element_text(&mut scan)?;
                        desc.have_description = true;
                    }
                    "Image" => {
                        desc.image_url = read_element_text(&mut scan)?;
                        desc.have_image_url = true;
                    }
                    "Url" => {
                        let typ = attr_or_empty(&attrs, "type").to_owned();
                        let url = attr_or_empty(&attrs, "template").to_owned();
                        let method = attr_or_empty(&attrs, "method").to_owned();
                        // Arora extension: <Url purpose="image"> is the
                        // image-search endpoint (SRCH04).
                        let purpose = attr_or_empty(&attrs, "purpose").to_owned();

                        // First-wins skip conditions, in the C++ order.
                        let skip = (purpose == "image" && desc.image.is_some())
                            || (typ == "application/x-suggestions+json"
                                && desc.suggestions.is_some())
                            || (purpose.is_empty()
                                && (typ.is_empty()
                                    || typ == "text/html"
                                    || typ == "application/xhtml+xml")
                                && desc.search.is_some());
                        if !skip && !url.is_empty() {
                            let params = scan_url_params(&mut scan)?;
                            let slot = UrlSlot {
                                template: url,
                                method,
                                params,
                            };
                            if purpose == "image" {
                                desc.image = Some(slot);
                            } else if typ == "application/x-suggestions+json" {
                                desc.suggestions = Some(slot);
                            } else if typ.is_empty()
                                || typ == "text/html"
                                || typ == "application/xhtml+xml"
                            {
                                desc.search = Some(slot);
                            }
                            // Other types (rss etc.) drop, as before.
                        }
                    }
                    _ => {}
                }
                if desc.complete() {
                    break;
                }
            }
            _ => {}
        }
    }
    Ok(desc)
}

fn slot_json(slot: &UrlSlot) -> Value {
    let params: Vec<Value> = slot
        .params
        .iter()
        .map(|(k, v)| json!([k, v]))
        .collect();
    json!({
        "template": slot.template,
        "method": slot.method,
        "params": params,
    })
}

/// rc_opensearch_parse body: descriptor bytes -> JSON field map.
pub fn opensearch(data: &[u8]) -> RcResult<Vec<u8>> {
    let desc = parse_opensearch(data)?;
    let mut root = Map::new();
    if desc.have_name {
        root.insert("name".into(), json!(desc.name));
    }
    if desc.have_description {
        root.insert("description".into(), json!(desc.description));
    }
    if desc.have_image_url {
        root.insert("imageUrl".into(), json!(desc.image_url));
    }
    if let Some(slot) = &desc.search {
        root.insert("search".into(), slot_json(slot));
    }
    if let Some(slot) = &desc.suggestions {
        root.insert("suggestions".into(), slot_json(slot));
    }
    if let Some(slot) = &desc.image {
        root.insert("image".into(), slot_json(slot));
    }
    Ok(Value::Object(root).to_string().into_bytes())
}

// ------------------------------------------------------------------
// gupdate extension-update manifests
// ------------------------------------------------------------------

/// rc_updatemanifest_parse body: manifest bytes -> {"offers":[...]}.
pub fn update_manifest(data: &[u8]) -> RcResult<Vec<u8>> {
    if data.len() > MAX_UPDATE_MANIFEST {
        return corrupt("the update manifest is too large");
    }
    let data = bom_transcode(data)?;
    let mut scan = Scan::new(&data);
    // The C++ loop's single-slot "current app": set by <app appid>,
    // cleared by </app>.  An <app> with no appid attribute is
    // inert — matching QString()'s null check.
    let mut current_app: Option<String> = None;
    let mut offers: Vec<Value> = Vec::new();
    let mut saw_element = false;
    loop {
        match scan.next()? {
            Ev::Eof => break,
            Ev::DocType => return corrupt("the update manifest must not contain a DTD"),
            Ev::Start { name, attrs } => {
                saw_element = true;
                match local(&name) {
                    "app" => {
                        current_app = attr(&attrs, "appid").map(str::to_owned);
                    }
                    "updatecheck" => {
                        if let Some(id) = &current_app {
                            offers.push(json!([
                                id,
                                attr_or_empty(&attrs, "status"),
                                attr_or_empty(&attrs, "codebase"),
                                attr_or_empty(&attrs, "version"),
                            ]));
                        }
                    }
                    _ => {}
                }
            }
            Ev::End { name } => {
                if local(&name) == "app" {
                    current_app = None;
                }
            }
            _ => {}
        }
    }
    if !saw_element {
        return corrupt("the update manifest is not valid XML");
    }
    Ok(json!({ "offers": offers }).to_string().into_bytes())
}

// ------------------------------------------------------------------
// OpenSearch suggestions replies
// ------------------------------------------------------------------

/// rc_suggest_parse body: reply bytes -> JSON array of strings.
/// RC_CORRUPT whenever the document is not the [term, [...]] shape —
/// the caller treats that as "no suggestions", identically to the old
/// QJsonDocument path's early returns.
pub fn suggestions(data: &[u8]) -> RcResult<Vec<u8>> {
    if data.len() > MAX_SUGGESTIONS {
        return corrupt("the suggestions reply is too large");
    }
    let doc: Value = serde_json::from_slice(data).map_err(|_| Fail {
        status: RcStatus::Corrupt,
        msg: "bad suggestions JSON".into(),
    })?;
    // ["term", ["s1", "s2", ...]] — QJsonValue::toString() on a
    // non-string produced "", keep that.
    let list = doc
        .as_array()
        .and_then(|parts| parts.get(1))
        .and_then(Value::as_array)
        .ok_or_else(|| Fail {
            status: RcStatus::Corrupt,
            msg: "suggestions reply is not the [term, [...]] shape".into(),
        })?;
    let out: Vec<&str> = list.iter().map(|v| v.as_str().unwrap_or("")).collect();
    Ok(json!(out).to_string().into_bytes())
}

// ------------------------------------------------------------------
// XBEL structural gate (bookmark load + import)
// ------------------------------------------------------------------

/// rc_xbel_check body: proves the contract XbelReader enforces —
/// well-formed XML rooted at <xbel> with an absent or "1.0" version
/// and no more than XBEL_MAX_DEPTH levels of nesting.  DOCTYPE is
/// tolerated (the XBEL spec ships one and KDE exports carry it).
/// Returns the unit type; failures carry the reader's message style.
pub fn xbel_check(data: &[u8]) -> RcResult<()> {
    if data.len() > MAX_XBEL {
        return corrupt("the XBEL document is too large");
    }
    let data = bom_transcode(data)?;
    let mut scan = Scan::new(&data);
    let mut rooted = false;
    loop {
        match scan.next()? {
            Ev::Eof => break,
            Ev::Start { name, attrs } => {
                if scan.depth > XBEL_MAX_DEPTH {
                    return corrupt("the XBEL document is nested too deeply");
                }
                if !rooted {
                    rooted = true;
                    let version = attr_or_empty(&attrs, "version");
                    if local(&name) != "xbel" || !(version.is_empty() || version == "1.0")
                    {
                        return corrupt("the file is not an XBEL version 1.0 file");
                    }
                }
            }
            _ => {}
        }
    }
    if !rooted {
        return corrupt("the file is not an XBEL version 1.0 file");
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;

    const NS: &str = "xmlns='http://a9.com/-/spec/opensearch/1.1/'";

    fn fields(json: &[u8]) -> Value {
        serde_json::from_slice(json).unwrap()
    }

    #[test]
    fn opensearch_basic() {
        let doc = format!(
            "<OpenSearchDescription {NS}>\
             <ShortName>Wiki</ShortName><Description>d</Description>\
             <Url method='post' type='text/html' template='http://w/s'/>\
             <Url method='get' type='application/x-suggestions+json' template='http://w/j'/>\
             <Image>i.png</Image></OpenSearchDescription>"
        );
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["name"], "Wiki");
        assert_eq!(v["description"], "d");
        assert_eq!(v["imageUrl"], "i.png");
        assert_eq!(v["search"]["template"], "http://w/s");
        assert_eq!(v["search"]["method"], "post");
        assert_eq!(v["suggestions"]["template"], "http://w/j");
        assert_eq!(v["suggestions"]["method"], "get");
        assert!(v.get("image").is_none());
    }

    #[test]
    fn opensearch_params_and_first_wins() {
        let doc = format!(
            "<OpenSearchDescription {NS}>\
             <Url type='text/html' template='http://a/1'>\
               <Param name='q' value='{{searchTerms}}'/>\
               <Parameter name='b' value='foo'/>\
               <Param name='' value='skip'/>\
               <Param name='noval' value=''/>\
             </Url>\
             <Url type='text/html' template='http://a/2'/>\
             <Url template='http://a/3'/></OpenSearchDescription>"
        );
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["search"]["template"], "http://a/1");
        assert_eq!(
            v["search"]["params"],
            json!([["q", "{searchTerms}"], ["b", "foo"]])
        );
        // suggestions slot was never filled -> key absent
        assert!(v.get("suggestions").is_none());
    }

    #[test]
    fn opensearch_purpose_image_and_unknown_type() {
        let doc = format!(
            "<OpenSearchDescription {NS}>\
             <Url type='application/rss+xml' template='http://a/rss'/>\
             <Url type='text/html' purpose='image' method='get' \
                template='http://a/img'/><Image>p.png</Image></OpenSearchDescription>"
        );
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["image"]["template"], "http://a/img");
        assert!(v.get("search").is_none()); // rss never filled it
    }

    #[test]
    fn opensearch_rejects() {
        // DTD
        assert!(opensearch(
            format!("<!DOCTYPE d [ <!ENTITY a 'x'> ]>\
                     <OpenSearchDescription {NS}><ShortName>x</ShortName>\
                     </OpenSearchDescription>")
                .as_bytes()
        )
        .is_err());
        // wrong root / missing namespace
        assert!(opensearch(b"<foo><bar/></foo>").is_err());
        assert!(opensearch(
            b"<OpenSearchDescription><ShortName>x</ShortName></OpenSearchDescription>"
        )
        .is_err());
        // truncated inside <Url>
        assert!(opensearch(
            format!("<OpenSearchDescription {NS}><Url template='http://a/'>")
                .as_bytes()
        )
        .is_err());
        // empty + oversized + garbage
        assert!(opensearch(b"").is_err());
        assert!(opensearch(&vec![b'x'; MAX_OPENSEARCH + 1]).is_err());
        // undeclared entity in text
        assert!(opensearch(
            format!("<OpenSearchDescription {NS}><ShortName>a&foo;b</ShortName>\
                     </OpenSearchDescription>")
                .as_bytes()
        )
        .is_err());
    }

    #[test]
    fn opensearch_entities_and_cdata() {
        let doc = format!(
            "<OpenSearchDescription {NS}>\
             <ShortName>a&amp;b&#x21;</ShortName>\
             <Url template='http://a/?q=1&amp;x=2'/>\
             <Description><![CDATA[c <raw> d]]></Description>\
             </OpenSearchDescription>"
        );
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["name"], "a&b!");
        assert_eq!(v["description"], "c <raw> d");
        assert_eq!(v["search"]["template"], "http://a/?q=1&x=2");
    }

    #[test]
    fn opensearch_prefixed_namespace() {
        let doc = "<os:OpenSearchDescription \
                   xmlns:os='http://a9.com/-/spec/opensearch/1.1/'>\
                   <os:ShortName>P</os:ShortName>\
                   <os:Url template='http://a/'/></os:OpenSearchDescription>";
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["name"], "P");
        assert_eq!(v["search"]["template"], "http://a/");
        // wrong URI under the right prefix still rejects
        let bad = "<os:OpenSearchDescription xmlns:os='http://nope/'>\
                   </os:OpenSearchDescription>";
        assert!(opensearch(bad.as_bytes()).is_err());
    }

    #[test]
    fn opensearch_nested_param_and_text() {
        // <Param> nested inside another element inside <Url> is
        // captured (flat scan); nested element inside a text field
        // is an error.
        let doc = format!(
            "<OpenSearchDescription {NS}>\
             <Url template='http://a/'><wrap><Param name='x' value='y'/>\
             </wrap></Url></OpenSearchDescription>"
        );
        let v = fields(&opensearch(doc.as_bytes()).unwrap());
        assert_eq!(v["search"]["params"], json!([["x", "y"]]));
        let bad = format!(
            "<OpenSearchDescription {NS}><ShortName>a<b>x</b></ShortName>\
             </OpenSearchDescription>"
        );
        assert!(opensearch(bad.as_bytes()).is_err());
    }

    #[test]
    fn opensearch_utf16_bom() {
        let text = format!(
            "<OpenSearchDescription {NS}><ShortName>U</ShortName>\
             <Url template='http://a/'/></OpenSearchDescription>"
        );
        let mut bytes: Vec<u8> = vec![0xFF, 0xFE];
        for u in text.encode_utf16() {
            bytes.extend_from_slice(&u.to_le_bytes());
        }
        let v = fields(&opensearch(&bytes).unwrap());
        assert_eq!(v["name"], "U");
    }

    #[test]
    fn update_manifest_basic() {
        let xml = "<gupdate protocol='2.0'>\
                   <app appid='abc'>\
                     <updatecheck status='ok' codebase='https://x/p.zip' \
                                  version='2.0.0'/>\
                   </app>\
                   <app appid='def'><updatecheck status='noupdate'/></app>\
                   </gupdate>";
        let v = fields(&update_manifest(xml.as_bytes()).unwrap());
        assert_eq!(
            v["offers"],
            json!([
                ["abc", "ok", "https://x/p.zip", "2.0.0"],
                ["def", "noupdate", "", ""]
            ])
        );
    }

    #[test]
    fn update_manifest_edges() {
        // updatecheck outside <app> is dropped; <app> without appid
        // hides its updatecheck; </app> clears the slot.
        let xml = "<gupdate>\
                   <updatecheck version='1'/><app><updatecheck version='2'/></app>\
                   <app appid='a'><updatecheck version='3'/></app>\
                   <updatecheck version='4'/></gupdate>";
        let v = fields(&update_manifest(xml.as_bytes()).unwrap());
        assert_eq!(v["offers"], json!([["a", "", "", "3"]]));
        // malformed + DTD + truncated
        assert!(update_manifest(b"<gupdate>").is_err());
        assert!(update_manifest(b"<!DOCTYPE g><gupdate/>").is_err());
        assert!(update_manifest(b"not xml").is_err());
    }

    #[test]
    fn suggest_parse() {
        assert_eq!(
            fields(&suggestions(b"[\"term\",[\"a\",\"b\"]]").unwrap()),
            json!(["a", "b"])
        );
        // non-strings become "", extra parts ignored
        assert_eq!(
            fields(&suggestions(b"[\"t\",[\"a\",1,null],9]").unwrap()),
            json!(["a", "", ""])
        );
        // wrong shapes / garbage -> Corrupt (caller emits nothing)
        for bad in [&b"{}"[..], b"[]", b"[\"t\"]", b"[\"t\",{}]", b"junk"] {
            assert!(suggestions(bad).is_err(), "{bad:?}");
        }
        assert!(suggestions(&vec![b'x'; MAX_SUGGESTIONS + 1]).is_err());
    }

    #[test]
    fn xbel_gate() {
        assert!(xbel_check(b"<xbel><folder><title>t</title></folder></xbel>").is_ok());
        assert!(xbel_check(b"<?xml version='1.0'?><xbel version='1.0'/>").is_ok());
        // spec DOCTYPE tolerated
        assert!(xbel_check(
            b"<!DOCTYPE xbel PUBLIC '+' 'xbel.dtd'><xbel><bookmark href='h'/></xbel>"
        )
        .is_ok());
        // wrong root / wrong version / no root
        assert!(xbel_check(b"<html/>").is_err());
        assert!(xbel_check(b"<xbel version='2.0'/>").is_err());
        assert!(xbel_check(b"").is_err());
        // malformed
        assert!(xbel_check(b"<xbel><folder></xbel>").is_err());
        // depth bound: 256 ok, 257 not
        let deep = |n: usize| -> Vec<u8> {
            let mut d = b"<xbel>".to_vec();
            d.extend("<f>".repeat(n).as_bytes());
            d.extend("</f>".repeat(n).as_bytes());
            d.extend(b"</xbel>");
            d
        };
        assert!(xbel_check(&deep(254)).is_ok());
        assert!(xbel_check(&deep(300)).is_err());
        // oversized
        assert!(xbel_check(&vec![b'x'; MAX_XBEL + 1]).is_err());
    }
}
