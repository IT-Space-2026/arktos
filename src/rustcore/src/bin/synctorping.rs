//! SYNC02 verification harness: drives the real `synctransport`
//! module (the same code the mw domain ops wrap) for the live
//! two-process legs in tools/synctor-e2e.sh.
//!
//!   synctorping addr <data_root>
//!       Print this device's sync endpoint (keystore-derived,
//!       offline — the same call `transport_status` reports).
//!   synctorping genkey <data_root> <peer_onion>
//!       Print our public discovery key for <peer_onion>
//!       (descriptor:x25519:… — the secret half stays in the
//!       keystore).
//!   synctorping serve <data_root> <nick=key> [...]
//!       Start the tor transport, authorize each nick=key client,
//!       and answer PING frames with PONG until killed.
//!   synctorping ping <data_root> <peer_onion>
//!       Start the transport, connect, send PING, expect PONG.
//!       Exits 0 on PING-OK, 3 on a refused/failed connect, 5 on a
//!       broken reply.
//!
//! <data_root> is the app's data root — sync/ lives under it exactly
//! as the mw ops lay it out.

use std::path::Path;
use std::process::exit;
use std::sync::Arc;
use std::time::{Duration, Instant};

use arktos_rustcore::synctransport::{self, SyncTransport, TransportEvent};

fn root(args: &[String], i: usize) -> &Path {
    Path::new(&args[i])
}

fn wait_frame(t: &Arc<dyn SyncTransport>, deadline: Duration) -> Option<(u64, Vec<u8>)> {
    let start = Instant::now();
    while start.elapsed() < deadline {
        for ev in t.poll() {
            match ev {
                TransportEvent::Frame { link, payload } => return Some((link, payload)),
                TransportEvent::Failed { reason } => {
                    eprintln!("[tp] transport failed: {reason}");
                }
                TransportEvent::LinkClosed { link, reason } => {
                    eprintln!("[tp] link {link} closed: {reason}");
                }
                TransportEvent::Ready => eprintln!("[tp] ready"),
                TransportEvent::Bootstrap(p) => {
                    if p % 25 == 0 {
                        eprintln!("[tp] bootstrap {p}%");
                    }
                }
                TransportEvent::Incoming { link, from } => {
                    eprintln!("[tp] incoming link {link} from {from}");
                }
            }
        }
        std::thread::sleep(Duration::from_millis(50));
    }
    None
}

fn main() {
    let args: Vec<String> = std::env::args().collect();
    if args.len() < 3 {
        eprintln!("usage: synctorping <addr|genkey|serve|ping> <data_root> [args]");
        exit(64);
    }
    match args[1].as_str() {
        "addr" => {
            match synctransport::tor::endpoint_for(&root(&args, 2).join(sync_dir())) {
                Ok(ep) => println!("{ep}"),
                Err(e) => {
                    eprintln!("addr failed: {e:?}");
                    exit(1);
                }
            }
        }
        "genkey" => {
            let peer = args.get(3).map(String::as_str).unwrap_or("");
            match synctransport::discovery_key(root(&args, 2), peer) {
                Ok(k) => println!("{k}"),
                Err(e) => {
                    eprintln!("genkey failed: {e:?}");
                    exit(1);
                }
            }
        }
        "serve" => {
            let t = match synctransport::start(root(&args, 2), "tor", "") {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("serve: transport start failed: {e:?}");
                    exit(1);
                }
            };
            for grant in &args[3..] {
                let Some((nick, key)) = grant.split_once('=') else {
                    eprintln!("serve: bad grant {grant} (want nick=key)");
                    exit(64);
                };
                if let Err(e) = t.authorize(nick, key) {
                    eprintln!("serve: authorize {nick}: {e:?}");
                    exit(1);
                }
            }
            let endpoint = t.endpoint().unwrap_or_default();
            println!("SERVING {endpoint}");
            eprintln!("[serve] up at {endpoint}");
            // Answer every PING with PONG until killed.
            loop {
                match wait_frame(&t, Duration::from_secs(3600)) {
                    Some((link, payload)) if payload == b"PING" => {
                        eprintln!("[serve] PING on link {link}");
                        if let Err(e) = t.send(link, b"PONG") {
                            eprintln!("[serve] send PONG: {e:?}");
                        } else {
                            println!("PING-SERVED");
                        }
                    }
                    Some((_, payload)) => {
                        eprintln!("[serve] stray frame {payload:?}");
                    }
                    None => break,
                }
            }
        }
        "ping" => {
            let peer = args.get(3).map(String::as_str).unwrap_or("");
            let t = match synctransport::start(root(&args, 2), "tor", "") {
                Ok(t) => t,
                Err(e) => {
                    eprintln!("ping: transport start failed: {e:?}");
                    exit(1);
                }
            };
            let link = match t.connect(peer, "e2e") {
                Ok(l) => l,
                Err(e) => {
                    println!("CONNECT-FAIL {e:?}");
                    exit(3);
                }
            };
            if let Err(e) = t.send(link, b"PING") {
                println!("SEND-FAIL {e:?}");
                exit(4);
            }
            match wait_frame(&t, Duration::from_secs(120)) {
                Some((_, payload)) if payload == b"PONG" => println!("PING-OK"),
                other => {
                    println!("REPLY-FAIL {other:?}");
                    exit(5);
                }
            }
        }
        other => {
            eprintln!("unknown mode {other}");
            exit(64);
        }
    }
}

/// `<data root>/sync` — the layout synctransport uses.
fn sync_dir() -> String {
    "sync".to_string()
}
