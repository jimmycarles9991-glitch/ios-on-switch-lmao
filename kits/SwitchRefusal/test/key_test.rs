// SPDX-License-Identifier: BSD-2-Clause-Patent
//! SwitchRefusal.rs's keys are the header's, bit for bit (key_shim.c calls its inline functions), folded at compile time
//! too; and `refuse` makes and writes its text once per key.

use std::ffi::{CString, c_char};

unsafe extern "C" {
    fn switch_refusal_test_key(domain: *const c_char, reason_key: *const c_char) -> u64;
    fn switch_refusal_test_key_with(key: u64, detail: u64) -> u64;
}

fn c_key(domain: &str, reason: &str) -> u64 {
    let (domain, reason) = (CString::new(domain).unwrap(), CString::new(reason).unwrap());
    // SAFETY: NUL-terminated strings, read only.
    unsafe { switch_refusal_test_key(domain.as_ptr(), reason.as_ptr()) }
}

const FOLDED: u64 = switch_refusal::key("m2v", "renderCommandEncoder");

#[test]
fn keys_are_the_headers() {
    for (domain, reason) in [
        ("m2v", "renderCommandEncoder"),
        ("", ""),
        ("raw", "pass.ds"),
        ("a", "b\u{e9}c"),
    ] {
        let key = c_key(domain, reason);
        assert_eq!(switch_refusal::key(domain, reason), key, "{domain} / {reason}");
        for detail in [0, 1, 542, u64::MAX, 0x0123_4567_89ab_cdef] {
            // SAFETY: plain values.
            assert_eq!(switch_refusal::key_with(key, detail), unsafe {
                switch_refusal_test_key_with(key, detail)
            });
        }
    }
    assert_eq!(FOLDED, c_key("m2v", "renderCommandEncoder"));
}

#[test]
fn refuse_writes_once() {
    let key = switch_refusal::key("m2v", "key_test");
    switch_refusal::refuse(c"m2v", "key_test", key, || "the first refusal".into());
    switch_refusal::refuse(c"m2v", "key_test", key, || panic!("a second line"));
}
