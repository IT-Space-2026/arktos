// EXT13 rquickjs host — see ../driver.rs for the measurement protocol.

use rquickjs::{Context, Runtime};

const ENGINE_NAME: &str = "qjs";

struct QjsEngine {
    rt: Runtime,
    ctx: Context,
}

fn new_engine() -> Result<QjsEngine, String> {
    let rt = Runtime::new().map_err(|e| format!("Runtime::new: {e}"))?;
    let ctx = Context::full(&rt).map_err(|e| format!("Context::full: {e}"))?;
    Ok(QjsEngine { rt, ctx })
}

impl Engine for QjsEngine {
    fn eval(&mut self, name: &str, src: &str) -> Result<Option<String>, String> {
        let name = name.to_string();
        let src = src.to_string();
        self.ctx.with(|ctx| {
            match ctx.eval::<rquickjs::Value, _>(src.as_str()) {
                Ok(v) => {
                    if v.is_string() {
                        Ok(Some(
                            v.as_string()
                                .and_then(|s| s.to_string().ok())
                                .unwrap_or_default(),
                        ))
                    } else if v.is_undefined() || v.is_null() {
                        Ok(None)
                    } else {
                        // non-string scalar — stringify in-JS via a global
                        // (rquickjs 0.10 has no json feature).
                        let set = ctx
                            .globals()
                            .set("__ext13_ret", v)
                            .map_err(|e| format!("{name}: set: {e}"));
                        if set.is_err() {
                            return set.map(|_| None);
                        }
                        ctx.eval::<String, _>(
                            "try{JSON.stringify(globalThis.__ext13_ret)}catch(e){String(globalThis.__ext13_ret)}",
                        )
                        .map(Some)
                        .map_err(|e| format!("{name}: stringify: {e}"))
                    }
                }
                Err(e) => {
                    // surface the engine's own exception text
                    let detail = ctx
                        .catch()
                        .as_exception()
                        .map(|ex| {
                            format!(
                                "{}{}",
                                ex.message().unwrap_or_default(),
                                ex.stack().unwrap_or_default()
                            )
                        });
                    Err(format!("{name}: {e} {detail:?}"))
                }
            }
        })
    }

    fn drain_jobs(&mut self) {
        while self.ctx.with(|ctx| ctx.execute_pending_job()) {}
    }
}

include!("../../driver.rs");
