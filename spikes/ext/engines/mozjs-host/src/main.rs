// EXT13 mozjs host — see ../driver.rs for the measurement protocol.
#![allow(non_snake_case)]

use mozjs::conversions::jsstr_to_string;
use mozjs::gc::Handle;
use mozjs::jsapi::OnNewGlobalHookOption;
use mozjs::jsval::UndefinedValue;
use mozjs::rooted;
use mozjs::rust::wrappers2::JS_NewGlobalObject;
use mozjs::rust::{
    evaluate_script, CompileOptionsWrapper, HandleObject, JSEngine, RealmOptions,
    RootedObjectVectorWrapper, Runtime, SIMPLE_GLOBAL_CLASS,
};
use std::ffi::CString;
use std::ptr::{self, NonNull};

const ENGINE_NAME: &str = "mozjs";

struct MozjsEngine {
    rt: Runtime,
    // Keeps the global object alive across eval calls — the vector is
    // a persistent GC root, and `global` is handed out through
    // Handle::from_marked_location (a raw borrow of the field, so the
    // handle's lifetime never pins &mut self).
    roots: RootedObjectVectorWrapper,
    global: *mut mozjs::jsapi::JSObject,
}

fn new_engine() -> Result<MozjsEngine, String> {
    let engine = JSEngine::init().map_err(|e| format!("JSEngine::init: {e:?}"))?;
    let mut rt = Runtime::new(engine.handle());
    // The JSEngine asserts on drop that every handle is released; the
    // runtime's handle outlives the engine object here, and JS_ShutDown
    // itself is teardown-crashy under SpikeMonkey — leak the singleton,
    // the process exits right after the measurement.
    std::mem::forget(engine);
    let roots = RootedObjectVectorWrapper::new(rt.cx());
    Ok(MozjsEngine {
        rt,
        roots,
        global: ptr::null_mut(),
    })
}

impl Engine for MozjsEngine {
    fn eval(&mut self, name: &str, src: &str) -> Result<Option<String>, String> {
        if self.global.is_null() {
            let realm = RealmOptions::default();
            rooted!(&in(self.rt.cx()) let g = unsafe {
                JS_NewGlobalObject(
                    self.rt.cx(),
                    &SIMPLE_GLOBAL_CLASS,
                    ptr::null_mut(),
                    OnNewGlobalHookOption::FireOnNewGlobalHook,
                    &*realm,
                )
            });
            if g.get().is_null() {
                return Err("JS_NewGlobalObject failed".into());
            }
            if !self.roots.append(g.get()) {
                return Err("root append failed".into());
            }
            self.global = g.get();
        }
        let global: HandleObject =
            unsafe { Handle::from_marked_location(&self.global) };
        rooted!(&in(self.rt.cx()) let mut rval = UndefinedValue());
        let fname = CString::new(name).unwrap_or_else(|_| CString::new("x.js").unwrap());
        let options = CompileOptionsWrapper::new(self.rt.cx_no_gc(), fname, 1);
        match evaluate_script(self.rt.cx(), global, src, rval.handle_mut(), options) {
            Ok(()) => {
                let v = rval.get();
                if v.is_string() {
                    let raw = v.to_string();
                    let s = unsafe {
                        jsstr_to_string(
                            self.rt.cx_no_gc(),
                            NonNull::new(raw).ok_or("null JSString")?,
                        )
                    };
                    Ok(Some(s))
                } else if v.is_int32() {
                    Ok(Some(v.to_int32().to_string()))
                } else if v.is_double() {
                    Ok(Some(v.to_double().to_string()))
                } else if v.is_boolean() {
                    Ok(Some(v.to_boolean().to_string()))
                } else {
                    Ok(None)
                }
            }
            Err(()) => {
                // Surface SpiderMonkey's pending exception text, not
                // just "eval threw".
                rooted!(&in(self.rt.cx()) let mut exc = UndefinedValue());
                let mut msg = format!("{name}: eval threw");
                unsafe {
                    if mozjs::rust::wrappers2::JS_GetPendingException(
                        self.rt.cx(),
                        exc.handle_mut(),
                    ) && exc.get().is_string()
                    {
                        let raw = exc.get().to_string();
                        if let Some(s) = NonNull::new(raw) {
                            msg = format!(
                                "{name}: {}",
                                jsstr_to_string(self.rt.cx_no_gc(), s)
                            );
                        }
                    }
                    mozjs::rust::wrappers2::JS_ClearPendingException(self.rt.cx());
                }
                Err(msg)
            }
        }
    }
}

include!("../../driver.rs");
