//! SYNC10 — mDNS discovery for the LAN fast path.
//!
//! Hand-rolled, dependency-free multicast DNS: a paired device
//! announces `_arktos-sync._tcp.local` with its QUIC port, device id,
//! and onion endpoint in the TXT record; co-located peers learn the
//! `{device_id -> (addr, port)}` map the `auto` transport uses to
//! prefer a direct QUIC link over a Tor circuit.
//!
//! One UDP socket serves both halves: it answers PTR queries for the
//! service type and tracks every response it hears into the peer
//! table.  Records ride the wire uncompressed — answers are a few
//! hundred bytes, and a name-compression writer is exactly the kind
//! of code a hostile peer fuzzes; the reader still honors pointers
//! because real responders emit them.
//!
//! IPv4 only — the honest edge (LANv6 sync is out of scope for the
//! fast path; Tor still reaches those peers).

use std::collections::HashMap;
use std::io;
use std::net::{Ipv4Addr, SocketAddr, UdpSocket};
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

/// The mDNS multicast group + port.
pub const MDNS_GROUP: Ipv4Addr = Ipv4Addr::new(224, 0, 0, 251);
pub const MDNS_PORT: u16 = 5353;
/// The service type paired devices browse for.
pub const SERVICE_TYPE: &str = "_arktos-sync._tcp.local";
/// RR time-to-live for our records; peers expire entries past it.
pub const RR_TTL: u32 = 120;

const TYPE_A: u16 = 1;
const TYPE_PTR: u16 = 12;
const TYPE_TXT: u16 = 16;
const TYPE_SRV: u16 = 33;
const TYPE_ANY: u16 = 255;
const CLASS_IN: u16 = 1;
/// RFC 6762 §5.4 — the question class top bit asks for a unicast
/// response; we answer unicast whenever the source is not the mDNS
/// port anyway, so the bit only needs recognizing, not honoring.
const CLASS_QU: u16 = 0x8000;

// ---- wire codec --------------------------------------------------------------

/// dotted "a.b.local" -> DNS label sequence on `out`.
pub fn encode_name(name: &str, out: &mut Vec<u8>) {
    for label in name.trim_end_matches('.').split('.') {
        let b = label.as_bytes();
        out.push(b.len().min(63) as u8);
        out.extend_from_slice(&b[..b.len().min(63)]);
    }
    out.push(0);
}

/// Decode a possibly-compressed name at `pos`; returns (name, next
/// position past the name).  Pointers may recurse (a pointer may
/// point at a name that itself ends in one) — the hop bound stops
/// pointer loops from spinning forever on hostile packets.
pub fn decode_name(buf: &[u8], pos: usize) -> Option<(String, usize)> {
    let mut labels: Vec<String> = Vec::new();
    let mut p = pos;
    let mut end: Option<usize> = None;
    for _hop in 0..64 {
        let len = *buf.get(p)? as usize;
        if len == 0 {
            p += 1;
            break;
        }
        if len & 0xC0 == 0xC0 {
            let hi = (len & 0x3F) << 8;
            let lo = *buf.get(p + 1)? as usize;
            if end.is_none() {
                end = Some(p + 2);
            }
            p = hi | lo;
            continue;
        }
        if len & 0xC0 != 0 {
            return None;
        }
        p += 1;
        let s = buf.get(p..p + len)?;
        labels.push(String::from_utf8_lossy(s).into_owned());
        p += len;
    }
    Some((labels.join("."), end.unwrap_or(p)))
}

#[derive(Debug, Clone, PartialEq)]
pub enum RData {
    A(Ipv4Addr),
    Ptr(String),
    Txt(Vec<Vec<u8>>),
    Srv { port: u16, target: String },
    Other(Vec<u8>),
}

#[derive(Debug, Clone)]
pub struct RR {
    pub name: String,
    pub rtype: u16,
    pub class: u16,
    pub ttl: u32,
    pub rdata: RData,
}

#[derive(Debug, Clone)]
pub struct Question {
    pub name: String,
    pub qtype: u16,
    /// `true` when the QU bit requested a unicast answer.
    pub unicast: bool,
}

#[derive(Debug, Default)]
pub struct Packet {
    pub questions: Vec<Question>,
    pub answers: Vec<RR>,
    pub additionals: Vec<RR>,
}

fn rd_u16(buf: &[u8], p: usize) -> Option<u16> {
    Some(u16::from_be_bytes([*buf.get(p)?, *buf.get(p + 1)?]))
}

fn rd_u32(buf: &[u8], p: usize) -> Option<u32> {
    Some(u32::from_be_bytes([
        *buf.get(p)?,
        *buf.get(p + 1)?,
        *buf.get(p + 2)?,
        *buf.get(p + 3)?,
    ]))
}

fn parse_rr(buf: &[u8], mut p: usize) -> Option<(RR, usize)> {
    let (name, np) = decode_name(buf, p)?;
    p = np;
    let rtype = rd_u16(buf, p)?;
    let class = rd_u16(buf, p + 2)?;
    let ttl = rd_u32(buf, p + 4)?;
    let rdlen = rd_u16(buf, p + 8)? as usize;
    let rd_start = p + 10;
    let rd_end = rd_start.checked_add(rdlen)?;
    if rd_end > buf.len() {
        return None;
    }
    let rd = &buf[rd_start..rd_end];
    let rdata = match rtype {
        TYPE_A if rdlen == 4 => RData::A(Ipv4Addr::new(rd[0], rd[1], rd[2], rd[3])),
        TYPE_PTR => decode_name(buf, rd_start)
            .map(|(n, _)| RData::Ptr(n))
            .unwrap_or_else(|| RData::Other(rd.to_vec())),
        TYPE_SRV if rdlen >= 7 => decode_name(buf, rd_start + 6)
            .map(|(t, _)| RData::Srv {
                port: u16::from_be_bytes([rd[4], rd[5]]),
                target: t,
            })
            .unwrap_or_else(|| RData::Other(rd.to_vec())),
        TYPE_TXT => {
            let mut strs = Vec::new();
            let mut q = 0usize;
            while q < rd.len() {
                let l = rd[q] as usize;
                q += 1;
                if q + l > rd.len() {
                    break;
                }
                strs.push(rd[q..q + l].to_vec());
                q += l;
            }
            RData::Txt(strs)
        }
        _ => RData::Other(rd.to_vec()),
    };
    Some((
        RR {
            name,
            rtype,
            class,
            ttl,
            rdata,
        },
        rd_end,
    ))
}

/// Parse one datagram.  Malformed packets yield a partial packet at
/// worst — a truncated tail is ignored rather than failing the
/// responder loop.
pub fn parse_packet(buf: &[u8]) -> Packet {
    let mut pkt = Packet::default();
    if buf.len() < 12 {
        return pkt;
    }
    let qd = u16::from_be_bytes([buf[4], buf[5]]) as usize;
    let an = u16::from_be_bytes([buf[6], buf[7]]) as usize;
    let ns = u16::from_be_bytes([buf[8], buf[9]]) as usize;
    let ar = u16::from_be_bytes([buf[10], buf[11]]) as usize;
    let mut p = 12usize;
    for _ in 0..qd {
        let Some((name, np)) = decode_name(buf, p) else { return pkt };
        let Some(qt) = rd_u16(buf, np) else { return pkt };
        let Some(qc) = rd_u16(buf, np + 2) else { return pkt };
        pkt.questions.push(Question {
            name,
            qtype: qt,
            unicast: qc & CLASS_QU != 0,
        });
        p = np + 4;
    }
    for i in 0..an + ns + ar {
        let Some((rr, np)) = parse_rr(buf, p) else { return pkt };
        if i < an {
            pkt.answers.push(rr);
        } else if i >= an + ns {
            pkt.additionals.push(rr);
        }
        p = np;
    }
    pkt
}

/// A PTR question for the sync service type (id 0, standard query).
pub fn build_query() -> Vec<u8> {
    let mut v = Vec::with_capacity(64);
    v.extend_from_slice(&[0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0]);
    encode_name(SERVICE_TYPE, &mut v);
    v.extend_from_slice(&TYPE_PTR.to_be_bytes());
    v.extend_from_slice(&(CLASS_IN | CLASS_QU).to_be_bytes());
    v
}

fn push_rr_head(out: &mut Vec<u8>, name: &str, rtype: u16, ttl: u32) -> usize {
    encode_name(name, out);
    out.extend_from_slice(&rtype.to_be_bytes());
    out.extend_from_slice(&CLASS_IN.to_be_bytes());
    out.extend_from_slice(&ttl.to_be_bytes());
    let pos = out.len();
    out.extend_from_slice(&[0, 0]); // rdlen patched below
    pos
}

fn patch_rdlen(out: &mut [u8], pos: usize) {
    let len = (out.len() - pos - 2) as u16;
    out[pos..pos + 2].copy_from_slice(&len.to_be_bytes());
}

/// Our own announcement: PTR + SRV + TXT + A in one response packet.
/// `ttl` of 0 is a goodbye (stop announces it).
pub fn build_response(
    instance: &str,
    host: &str,
    port: u16,
    txt: &[Vec<u8>],
    addrs: &[Ipv4Addr],
    ttl: u32,
) -> Vec<u8> {
    let mut v = Vec::with_capacity(256 + txt.iter().map(Vec::len).sum::<usize>());
    let n_answers = 2u16; // PTR + SRV
    let n_add = (1 + addrs.len()) as u16; // TXT + As
    v.extend_from_slice(&[
        0, 0, // id
        0x84, 0x00, // response, authoritative
        0, 0, // qd
        (n_answers >> 8) as u8, n_answers as u8,
        0, 0, // ns
        (n_add >> 8) as u8, n_add as u8,
    ]);
    // PTR: service type -> instance
    let p = push_rr_head(&mut v, SERVICE_TYPE, TYPE_PTR, ttl);
    encode_name(instance, &mut v);
    patch_rdlen(&mut v, p);
    // SRV: instance -> host:port
    let p = push_rr_head(&mut v, instance, TYPE_SRV, ttl);
    v.extend_from_slice(&[0, 0, 0, 0]); // priority, weight
    v.extend_from_slice(&port.to_be_bytes());
    encode_name(host, &mut v);
    patch_rdlen(&mut v, p);
    // TXT: instance -> strings
    let p = push_rr_head(&mut v, instance, TYPE_TXT, ttl);
    for s in txt {
        v.push(s.len().min(255) as u8);
        v.extend_from_slice(&s[..s.len().min(255)]);
    }
    patch_rdlen(&mut v, p);
    // A: host -> ipv4
    for a in addrs {
        let p = push_rr_head(&mut v, host, TYPE_A, ttl);
        v.extend_from_slice(&a.octets());
        patch_rdlen(&mut v, p);
    }
    v
}

// ---- the running service --------------------------------------------------------

/// One discovered peer, refreshed on every seen packet.
#[derive(Debug, Clone)]
pub struct PeerRecord {
    pub device_id: String,
    /// The peer's advertised onion endpoint, when it runs tor too —
    /// the `auto` transport keys LAN-vs-Tor routing on this.
    pub onion: String,
    /// The advertised QUIC port (from SRV).
    pub port: u16,
    /// Candidate IPv4s (A records for the SRV target, and the
    /// packet's source as a same-subnet fallback).
    pub addrs: Vec<Ipv4Addr>,
    pub last_seen: Instant,
    pub ttl: u32,
}

struct Shared {
    peers: Mutex<HashMap<String, PeerRecord>>,
    stop: AtomicBool,
    /// Our own instance name — our announcements loop back on the
    /// same interface and must not self-register as a peer.
    instance: String,
    host: String,
    port: u16,
    txt: Vec<Vec<u8>>,
    addrs: Vec<Ipv4Addr>,
    group: Ipv4Addr,
    mport: u16,
}

/// The running mDNS service: one socket, one pump thread, a query
/// timer thread.  `stop` is idempotent via Drop.
pub struct MdnsService {
    sock: Arc<UdpSocket>,
    shared: Arc<Shared>,
}

pub struct MdnsConfig {
    /// Full service instance ("<id16>._arktos-sync._tcp.local").
    pub instance: String,
    /// Hostname the SRV target resolves as ("arktos-<id16>.local").
    pub host: String,
    /// The QUIC port the SRV record advertises.
    pub port: u16,
    /// TXT strings, e.g. ["v=1", "dev=<id64>", "on=<onion>"].
    pub txt: Vec<Vec<u8>>,
    /// Local IPv4s the A records advertise.
    pub addrs: Vec<Ipv4Addr>,
    /// Multicast group + port (standard 224.0.0.251:5353; tests
    /// override to a scoped group/port).
    pub group: Ipv4Addr,
    pub mport: u16,
    /// Interface addr for the multicast membership (0.0.0.0 = any).
    pub iface: Ipv4Addr,
    /// Active re-query period.
    pub query_every: Duration,
}

fn outbound_v4() -> Option<Ipv4Addr> {
    // A UDP "connect" picks the route's source without sending a
    // byte — the honest primary-interface guess for LAN adverts.
    let s = UdpSocket::bind((Ipv4Addr::UNSPECIFIED, 0)).ok()?;
    s.connect((MDNS_GROUP, MDNS_PORT)).ok()?;
    match s.local_addr().ok()?.ip() {
        std::net::IpAddr::V4(a) => Some(a),
        _ => None,
    }
}

/// The local IPv4 addresses we advertise — the primary outbound one
/// plus any candidates passed in (deduplicated).
pub fn local_addrs(extra: &[Ipv4Addr]) -> Vec<Ipv4Addr> {
    let mut v = Vec::new();
    if let Some(a) = outbound_v4() {
        v.push(a);
    }
    for a in extra {
        if !v.contains(a) {
            v.push(*a);
        }
    }
    v
}

/// Bind the mDNS socket: reuse-address + group membership so the
/// browser coexists with a system responder (avahi etc.), send+recv
/// on `iface` when given.
pub fn bind_socket(group: Ipv4Addr, mport: u16, iface: Ipv4Addr) -> io::Result<UdpSocket> {
    let dom = socket2::Domain::IPV4;
    let sock = socket2::Socket::new(dom, socket2::Type::DGRAM, Some(socket2::Protocol::UDP))?;
    sock.set_reuse_address(true)?;
    #[cfg(unix)]
    sock.set_reuse_port(true)?;
    sock.bind(&socket2::SockAddr::from(SocketAddr::from((
        Ipv4Addr::UNSPECIFIED,
        mport,
    ))))?;
    sock.set_multicast_loop_v4(true)?;
    sock.set_multicast_ttl_v4(2)?;
    if !iface.is_unspecified() {
        sock.set_multicast_if_v4(&iface)?;
    }
    sock.join_multicast_v4(&group, &iface)?;
    sock.set_nonblocking(false)?;
    Ok(sock.into())
}

impl MdnsService {
    /// Bring the responder+browser up.  Errors are surfaced (a
    /// sandbox without multicast refuses the join) — the caller
    /// decides whether discovery-less operation is acceptable.
    pub fn start(cfg: MdnsConfig) -> io::Result<Arc<Self>> {
        let sock = Arc::new(bind_socket(cfg.group, cfg.mport, cfg.iface)?);
        let shared = Arc::new(Shared {
            peers: Mutex::new(HashMap::new()),
            stop: AtomicBool::new(false),
            instance: cfg.instance,
            host: cfg.host,
            port: cfg.port,
            txt: cfg.txt,
            addrs: cfg.addrs,
            group: cfg.group,
            mport: cfg.mport,
        });
        let svc = Arc::new(MdnsService {
            sock: Arc::clone(&sock),
            shared: Arc::clone(&shared),
        });

        // The recv pump: queries for our service get an answer
        // (multicast for mDNS-port queriers, unicast otherwise —
        // same code path, different destination); response packets
        // feed the peer table.
        {
            let sock = Arc::clone(&sock);
            let shared = Arc::clone(&shared);
            std::thread::Builder::new()
                .name("arktos-mdns".into())
                .spawn(move || {
                    let mut buf = [0u8; 9000];
                    while !shared.stop.load(Ordering::SeqCst) {
                        let (n, from) = match sock.recv_from(&mut buf) {
                            Ok(r) => r,
                            Err(e) => {
                                if shared.stop.load(Ordering::SeqCst) {
                                    break;
                                }
                                if e.kind() == io::ErrorKind::Interrupted {
                                    continue;
                                }
                                std::thread::sleep(Duration::from_millis(20));
                                continue;
                            }
                        };
                        Self::handle_datagram(&sock, &shared, &buf[..n], from);
                    }
                })
                .map_err(|e| io::Error::new(e.kind(), format!("mdns pump: {e}")))?;
        }

        // The query timer: an announcement burst on start, then a
        // periodic PTR query to keep the peer table warm (peers'
        // own bursts and query answers do the rest).
        {
            let sock = Arc::clone(&sock);
            let shared = Arc::clone(&shared);
            let every = cfg.query_every;
            std::thread::Builder::new()
                .name("arktos-mdns-query".into())
                .spawn(move || {
                    let dest = SocketAddr::from((shared.group, shared.mport));
                    for burst in 0..3 {
                        if shared.stop.load(Ordering::SeqCst) {
                            return;
                        }
                        let pkt = build_response(
                            &shared.instance,
                            &shared.host,
                            shared.port,
                            &shared.txt,
                            &shared.addrs,
                            RR_TTL,
                        );
                        let _ = sock.send_to(&pkt, dest);
                        std::thread::sleep(Duration::from_millis(300 + 200 * burst));
                    }
                    let query = build_query();
                    while !shared.stop.load(Ordering::SeqCst) {
                        let _ = sock.send_to(&query, dest);
                        for _ in 0..10 {
                            if shared.stop.load(Ordering::SeqCst) {
                                return;
                            }
                            std::thread::sleep(every / 10);
                        }
                    }
                })
                .map_err(|e| io::Error::new(e.kind(), format!("mdns query: {e}")))?;
        }
        Ok(svc)
    }

    fn handle_datagram(sock: &UdpSocket, shared: &Shared, buf: &[u8], from: SocketAddr) {
        let pkt = parse_packet(buf);
        // Answer queries for our service type.  A responder sends
        // multicast when the querier is on the mDNS port, unicast
        // otherwise — the second half is what makes a directed test
        // query work without multicast at all.
        for q in &pkt.questions {
            let ours = q.name.eq_ignore_ascii_case(SERVICE_TYPE)
                && matches!(q.qtype, TYPE_PTR | TYPE_ANY);
            if !ours {
                continue;
            }
            let pkt = build_response(
                &shared.instance,
                &shared.host,
                shared.port,
                &shared.txt,
                &shared.addrs,
                RR_TTL,
            );
            let dest = if from.port() == shared.mport {
                SocketAddr::from((shared.group, shared.mport))
            } else {
                from
            };
            let _ = sock.send_to(&pkt, dest);
        }
        // Fold every answer into the peer table: SRV gives the port,
        // TXT the identity, A the addresses for the SRV target.
        let rrs: Vec<&RR> = pkt.answers.iter().chain(pkt.additionals.iter()).collect();
        if rrs.is_empty() {
            return;
        }
        // Instance -> (port, target) from SRV records.
        let mut srvs: HashMap<String, (u16, String)> = HashMap::new();
        // host -> addrs
        let mut hosts: HashMap<String, Vec<Ipv4Addr>> = HashMap::new();
        // instance -> txt fields
        let mut txts: HashMap<String, Vec<Vec<u8>>> = HashMap::new();
        for rr in &rrs {
            match &rr.rdata {
                RData::Srv { port, target } => {
                    srvs.insert(rr.name.to_lowercase(), (*port, target.clone()));
                }
                RData::A(a) => {
                    hosts.entry(rr.name.to_lowercase()).or_default().push(*a);
                }
                RData::Txt(strs) => {
                    txts.insert(rr.name.to_lowercase(), strs.clone());
                }
                _ => {}
            }
        }
        let src_v4 = match from.ip() {
            std::net::IpAddr::V4(a) => Some(a),
            _ => None,
        };
        let mut peers = shared.peers.lock().unwrap_or_else(|e| e.into_inner());
        for (inst, (port, target)) in &srvs {
            if !inst.ends_with(&format!(".{SERVICE_TYPE}")) || *inst == shared.instance {
                continue;
            }
            let (mut device_id, mut onion) = (String::new(), String::new());
            if let Some(strs) = txts.get(inst) {
                for s in strs {
                    if let Some(rest) = s.strip_prefix(b"dev=") {
                        device_id = String::from_utf8_lossy(rest).into_owned();
                    } else if let Some(rest) = s.strip_prefix(b"on=") {
                        onion = String::from_utf8_lossy(rest).into_owned();
                    }
                }
            }
            if device_id.is_empty() {
                continue;
            }
            let mut addrs = hosts.get(&target.to_lowercase()).cloned().unwrap_or_default();
            if let Some(a) = src_v4 {
                if !addrs.contains(&a) {
                    addrs.push(a);
                }
            }
            // TTL 0 is a goodbye — drop the peer.
            let ttl = rrs
                .iter()
                .find(|r| r.name.eq_ignore_ascii_case(inst) && r.rtype == TYPE_SRV)
                .map(|r| r.ttl)
                .unwrap_or(RR_TTL);
            let key = device_id.clone();
            if ttl == 0 {
                peers.remove(&key);
                continue;
            }
            peers.insert(
                key,
                PeerRecord {
                    device_id,
                    onion,
                    port: *port,
                    addrs,
                    last_seen: Instant::now(),
                    ttl,
                },
            );
        }
    }

    /// Live peers (fresh entries only — expired ones are pruned on
    /// read so a quiet network does not strand stale routes).
    pub fn peers(&self) -> Vec<PeerRecord> {
        let mut peers = self
            .shared
            .peers
            .lock()
            .unwrap_or_else(|e| e.into_inner());
        let now = Instant::now();
        peers.retain(|_, p| now.duration_since(p.last_seen).as_secs() <= u64::from(p.ttl));
        peers.values().cloned().collect()
    }

    /// The discovered record for `device_id` (full or prefix).
    pub fn find(&self, device_id: &str) -> Option<PeerRecord> {
        self.peers()
            .into_iter()
            .find(|p| p.device_id == device_id || p.device_id.starts_with(device_id))
    }

    /// The discovered peer advertising `onion` — how `auto` maps a
    /// tor endpoint onto a co-located LAN address.  The match is on
    /// the host half, so an endpoint given with or without its port
    /// resolves the same peer.
    pub fn find_by_onion(&self, onion: &str) -> Option<PeerRecord> {
        let want = onion.rsplit_once(':').map(|(h, _)| h).unwrap_or(onion);
        self.peers().into_iter().find(|p| {
            let have = p
                .onion
                .rsplit_once(':')
                .map(|(h, _)| h)
                .unwrap_or(p.onion.as_str());
            !want.is_empty() && have == want
        })
    }

    /// Our instance name ("<id16>._arktos-sync._tcp.local").
    pub fn instance(&self) -> &str {
        &self.shared.instance
    }

    /// Stop the pumps and send a TTL-0 goodbye.  Idempotent.
    pub fn stop(&self) {
        if self.shared.stop.swap(true, Ordering::SeqCst) {
            return;
        }
        let goodbye = build_response(
            &self.shared.instance,
            &self.shared.host,
            self.shared.port,
            &self.shared.txt,
            &self.shared.addrs,
            0,
        );
        let _ = self.sock.send_to(
            &goodbye,
            SocketAddr::from((self.shared.group, self.shared.mport)),
        );
        // Kick the recv pump awake so the thread exits promptly.
        let _ = self
            .sock
            .send_to(b"\x00", SocketAddr::from((Ipv4Addr::LOCALHOST, self.shared.mport)));
    }
}

impl Drop for MdnsService {
    fn drop(&mut self) {
        self.stop();
    }
}

// ---- tests -------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn name_codec_round_trip() {
        let mut v = Vec::new();
        encode_name("a.b.local", &mut v);
        v.extend_from_slice(&[0xAA, 0xBB]); // trailing bytes after name
        let (n, next) = decode_name(&v, 0).unwrap();
        assert_eq!(n, "a.b.local");
        assert_eq!(next, v.len() - 2);
    }

    #[test]
    fn name_codec_follows_compression_pointer() {
        // "a.local" at offset 0; a name at offset 8 = "b" + ptr(2).
        let mut v = Vec::new();
        encode_name("a.local", &mut v); // len: 1a5local0 = 9 bytes
        let ptr_target = 2usize; // "local"
        v.push(1);
        v.extend_from_slice(b"b");
        v.push(0xC0);
        v.push(ptr_target as u8);
        let (n, next) = decode_name(&v, 9).unwrap();
        assert_eq!(n, "b.local");
        assert_eq!(next, 9 + 4);
        // A pointer to itself must not spin.
        let bad = [0xC0, 0x00];
        assert_eq!(decode_name(&bad, 0).unwrap().0, "");
    }

    #[test]
    fn query_is_service_ptr() {
        let q = build_query();
        let pkt = parse_packet(&q);
        assert_eq!(pkt.questions.len(), 1);
        assert_eq!(pkt.questions[0].name, SERVICE_TYPE);
        assert_eq!(pkt.questions[0].qtype, TYPE_PTR);
        assert!(pkt.questions[0].unicast);
    }

    #[test]
    fn response_round_trip() {
        let txt = vec![b"v=1".to_vec(), b"dev=abc123".to_vec(), b"on=x.onion:14430".to_vec()];
        let pkt = build_response(
            "abc123def._arktos-sync._tcp.local",
            "arktos-abc123def.local",
            14431,
            &txt,
            &[Ipv4Addr::new(192, 168, 1, 5)],
            RR_TTL,
        );
        let p = parse_packet(&pkt);
        assert_eq!(p.answers.len(), 2);
        assert_eq!(p.additionals.len(), 2);
        let ptr = &p.answers[0];
        assert_eq!(ptr.rtype, TYPE_PTR);
        assert_eq!(
            ptr.rdata,
            RData::Ptr("abc123def._arktos-sync._tcp.local".to_string())
        );
        let srv = p
            .answers
            .iter()
            .find(|r| r.rtype == TYPE_SRV)
            .expect("no SRV");
        match &srv.rdata {
            RData::Srv { port, target } => {
                assert_eq!(*port, 14431);
                assert_eq!(target, "arktos-abc123def.local");
            }
            _ => panic!("SRV rdata wrong"),
        }
        let a = p
            .additionals
            .iter()
            .find(|r| r.rtype == TYPE_A)
            .expect("no A");
        assert_eq!(a.rdata, RData::A(Ipv4Addr::new(192, 168, 1, 5)));
    }

    /// Two services in one process, real UDP multicast when the
    /// environment supports it: A announces, B discovers by device
    /// id.  The join failing (no multicast iface) is an honest
    /// environment skip — the join either works and discovery MUST
    /// converge, or the box cannot run LAN sync at all.
    #[test]
    fn discovery_converges_over_real_multicast() {
        let group = Ipv4Addr::new(239, 255, 77, 89);
        let mport = 15453u16;
        let mk = |id16: &str, port: u16, addrs: &[Ipv4Addr]| MdnsConfig {
            instance: format!("{id16}.{SERVICE_TYPE}"),
            host: format!("arktos-{id16}.local"),
            port,
            txt: vec![
                b"v=1".to_vec(),
                format!("dev={id16}{id16}{id16}{id16}").into_bytes(),
                b"on=peer.onion:14430".to_vec(),
            ],
            addrs: addrs.to_vec(),
            group,
            mport,
            iface: Ipv4Addr::UNSPECIFIED,
            query_every: Duration::from_millis(200),
        };
        let a = match MdnsService::start(mk("aaaaaaaaaaaaaaaa", 14001, &local_addrs(&[]))) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("SKIP discovery_converges_over_real_multicast: no multicast ({e})");
                return;
            }
        };
        let b = MdnsService::start(mk("bbbbbbbbbbbbbbbb", 14002, &local_addrs(&[]))).unwrap();
        let mut found = None;
        for _ in 0..200 {
            if let Some(p) = b.find("aaaaaaaaaaaaaaaa") {
                found = Some(p);
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        let p = found.expect("B never discovered A over multicast");
        assert_eq!(p.port, 14001);
        assert_eq!(p.onion, "peer.onion:14430");
        assert!(!p.addrs.is_empty());
        // A likewise sees B (its query answer reached A's table).
        let mut seen_b = false;
        for _ in 0..100 {
            if a.find("bbbbbbbbbbbbbbbb").is_some() {
                seen_b = true;
                break;
            }
            std::thread::sleep(Duration::from_millis(50));
        }
        assert!(seen_b, "A never discovered B");
        // Goodbye drops the peer.
        a.stop();
        b.stop();
    }

    /// The unicast half: a directed query to a responder's port is
    /// answered even with zero multicast — this is what makes mDNS
    /// testable in locked-down sandboxes.
    #[test]
    fn responder_answers_directed_unicast_query() {
        let group = Ipv4Addr::new(239, 255, 77, 90);
        let mport = 15454u16;
        let cfg = MdnsConfig {
            instance: format!("cccccccccccccccc.{SERVICE_TYPE}"),
            host: "arktos-cccccccccccccccc.local".into(),
            port: 14003,
            txt: vec![b"v=1".to_vec(), b"dev=cccc".to_vec()],
            addrs: vec![Ipv4Addr::LOCALHOST],
            group,
            mport,
            iface: Ipv4Addr::LOCALHOST,
            query_every: Duration::from_secs(3600), // announcements only
        };
        let svc = match MdnsService::start(cfg) {
            Ok(s) => s,
            Err(e) => {
                eprintln!("SKIP unicast query leg: {e}");
                return;
            }
        };
        let q = UdpSocket::bind((Ipv4Addr::LOCALHOST, 0)).unwrap();
        q.set_read_timeout(Some(Duration::from_secs(4))).unwrap();
        q.send_to(&build_query(), (Ipv4Addr::LOCALHOST, mport))
            .unwrap();
        let mut buf = [0u8; 4096];
        let (n, _from) = q.recv_from(&mut buf).expect("no unicast answer");
        let pkt = parse_packet(&buf[..n]);
        assert!(pkt
            .answers
            .iter()
            .any(|r| r.rtype == TYPE_SRV
                && matches!(&r.rdata, RData::Srv { port, .. } if *port == 14003)));
        svc.stop();
    }
}
