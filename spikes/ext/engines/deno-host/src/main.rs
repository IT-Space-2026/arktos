// EXT13 deno_core host — see ../driver.rs for the measurement protocol.

use deno_core::{JsRuntime, RuntimeOptions};

const ENGINE_NAME: &str = "deno";

struct DenoEngine {
    rt: JsRuntime,
}

fn new_engine() -> Result<DenoEngine, String> {
    Ok(DenoEngine {
        rt: JsRuntime::new(RuntimeOptions::default()),
    })
}

impl Engine for DenoEngine {
    fn eval(&mut self, name: &str, src: &str) -> Result<Option<String>, String> {
        let res = self
            .rt
            .execute_script(name.to_string(), src.to_string())
            .map_err(|e| format!("{name}: {e}"))?;
        deno_core::scope!(scope, &mut self.rt);
        let local = deno_core::v8::Local::new(scope, res);
        if local.is_undefined() || local.is_null() {
            Ok(None)
        } else {
            Ok(Some(local.to_rust_string_lossy(scope)))
        }
    }

    fn drain_jobs(&mut self) {
        let _ = self
            .rt
            .run_event_loop(deno_core::PollEventLoopOptions::default());
    }
}

include!("../../driver.rs");
