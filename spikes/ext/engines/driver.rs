// EXT13 Arch-B shared spike driver — include!()'d by each engine's
// main.rs so all runtimes run the identical measurement protocol:
//
//   1. eval the chrome-shim prelude (full mode) + web-globals stubs
//   2. install the payload script text as globalThis.__payload
//   3. __runPayload — indirect eval under try/catch (parse + eval result)
//   4. __fireMessage — push a message through registered onMessage
//      listeners and collect sendResponse payloads (the round-trip)
//   5. drain the fake timer queue
//   6. __report — the compat surface (which chrome.* / web APIs it hit)
//
// Output: `jshost:`-prefixed lines; exit 0 payload ok, 2 payload threw,
// 3 harness failure, 64/66 arg/io errors.

const CHROME_SHIM: &str = include_str!("../common/chrome-shim.js");
const SW_GLOBALS: &str = include_str!("../common/sw-globals.js");

trait Engine {
    /// Evaluate `src` (file label `name`); Ok(Some) when the completion
    /// value is a string/number we can print, Ok(None) for undefined,
    /// Err on parse/eval failure with the engine's message.
    fn eval(&mut self, name: &str, src: &str) -> Result<Option<String>, String>;
    /// Optional microtask/event-loop drain — engines with real job
    /// queues (deno_core, quickjs) override; the fake-timer queue is
    /// handled JS-side either way.
    fn drain_jobs(&mut self) {}
}

fn prelude_source() -> String {
    format!(
        "globalThis.__CHROME_SHIM_MODE = 'full';\n{}\n{}",
        CHROME_SHIM, SW_GLOBALS
    )
}

/// serde_json string literals are valid JS except for raw U+2028/2029.
fn payload_assignment(src: &str) -> String {
    let lit = serde_json::to_string(src)
        .expect("payload stringify")
        .replace('\u{2028}', "\\u2028")
        .replace('\u{2029}', "\\u2029");
    format!("globalThis.__payload = {lit};")
}

fn run_protocol<E: Engine>(eng: &mut E, engine_name: &str, src: &str, label: &str) -> i32 {
    if let Err(e) = eng.eval("shim.js", &prelude_source()) {
        println!("jshost: engine={engine_name} shim=FAIL {e}");
        return 3;
    }
    println!("jshost: engine={engine_name} shim=ok");

    if let Err(e) = eng.eval("payload.js", &payload_assignment(src)) {
        println!("jshost: payload {label} ok=false error=assign:{e}");
        return 2;
    }
    let mut rc = 2;
    let run_call = if std::env::var_os("EXT13_PARSE_ONLY").is_some() {
        format!("JSON.stringify(__parseOnly(__payload, {label:?}))")
    } else {
        format!("JSON.stringify(__runPayload(__payload, {label:?}))")
    };
    match eng.eval("run.js", &run_call) {
        Ok(Some(json)) => {
            println!("jshost: payload {label} {json}");
            if serde_json::from_str::<serde_json::Value>(&json)
                .map(|v| v["ok"].as_bool() == Some(true))
                .unwrap_or(false)
            {
                rc = 0;
            }
        }
        other => println!("jshost: payload {label} ok=? {other:?}"),
    }

    eng.drain_jobs();

    if let Ok(Some(json)) = eng.eval(
        "fire.js",
        "JSON.stringify(__fireMessage({ping:true,via:'rust-host'}))",
    ) {
        println!("jshost: fireMessage -> {json}");
    }
    if let Ok(Some(n)) = eng.eval(
        "drain.js",
        "String(__drainTimers ? __drainTimers(500) : -1)",
    ) {
        println!("jshost: timersDrained={n}");
    }
    // Async sendResponse replies land during the drain — re-read.
    if let Ok(Some(json)) = eng.eval(
        "fired.js",
        "JSON.stringify(globalThis.__fireResponses||[])",
    ) {
        println!("jshost: fireResponses(post-drain) -> {json}");
    }
    match eng.eval("report.js", "__report()") {
        Ok(Some(json)) => println!("jshost: report={json}"),
        other => println!("jshost: report=unavailable {other:?}"),
    }
    rc
}

fn main() {
    let mut script = String::new();
    let mut label = String::from("payload");
    let mut args = std::env::args().skip(1);
    while let Some(a) = args.next() {
        match a.as_str() {
            "--script" => script = args.next().unwrap_or_default(),
            "--label" => label = args.next().unwrap_or_default(),
            _ => {
                eprintln!("{ENGINE_NAME}: unknown arg {a}");
                std::process::exit(64);
            }
        }
    }
    if script.is_empty() {
        eprintln!("usage: {ENGINE_NAME} --script PATH [--label L]");
        std::process::exit(64);
    }
    let src = match std::fs::read_to_string(&script) {
        Ok(s) => s,
        Err(e) => {
            eprintln!("{ENGINE_NAME}: cannot read {script}: {e}");
            std::process::exit(66);
        }
    };
    eprintln!(
        "jshost: engine={} script={} bytes={}",
        ENGINE_NAME,
        script,
        src.len()
    );
    let mut eng = match new_engine() {
        Ok(e) => e,
        Err(e) => {
            eprintln!("jshost: {ENGINE_NAME} init failed: {e}");
            std::process::exit(3);
        }
    };
    let rc = run_protocol(&mut eng, ENGINE_NAME, &src, &label);
    // Engine teardown is out of scope for the spike — SpiderMonkey's
    // JS_DestroyContext hits a known mutex-destroy segfault at exit, and
    // the engine registers atexit/static destructors that fault even
    // when the runtime is never dropped.  _exit bypasses all of it;
    // println! is line-buffered so nothing is lost.
    std::mem::forget(eng);
    unsafe { libc::_exit(rc) }
}
