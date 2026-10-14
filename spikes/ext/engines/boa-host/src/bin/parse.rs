use boa_engine::{Context, Source};
fn main() {
    let path = std::env::args().nth(1).expect("usage: parse FILE");
    let src = std::fs::read_to_string(&path).unwrap();
    let mut ctx = Context::default();
    match ctx.eval(Source::from_bytes(&src)) {
        Ok(v) => println!("OK {:?}", v.display()),
        Err(e) => println!("ERR {e}"),
    }
}
