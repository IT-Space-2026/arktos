// EXT13 boa host — see ../driver.rs for the measurement protocol.

use boa_engine::{Context, Source};

const ENGINE_NAME: &str = "boa";

struct BoaEngine {
    ctx: Context,
}

fn new_engine() -> Result<BoaEngine, String> {
    Ok(BoaEngine {
        ctx: Context::default(),
    })
}

impl Engine for BoaEngine {
    fn eval(&mut self, name: &str, src: &str) -> Result<Option<String>, String> {
        match self.ctx.eval(Source::from_bytes(src)) {
            Ok(v) => {
                if v.is_string() {
                    Ok(v.as_string().map(|s| s.to_std_string_escaped()))
                } else if v.is_number() || v.is_boolean() {
                    Ok(Some(
                        v.to_string(&mut self.ctx)
                            .map(|s| s.to_std_string_escaped())
                            .unwrap_or_default(),
                    ))
                } else {
                    Ok(None)
                }
            }
            Err(e) => Err(format!("{name}: {e}")),
        }
    }
}

include!("../../driver.rs");
