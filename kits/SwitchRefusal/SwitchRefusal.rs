// SPDX-License-Identifier: BSD-2-Clause-Patent
//! The Rust side of SwitchRefusal.h: the keys as const fns, bit for bit the header's, and the C calls (SwitchRefusal.c,
//! linked once into the image that carries this crate).

use std::ffi::{CStr, CString, c_char};

const OFFSET: u64 = 0xcbf2_9ce4_8422_2325;
const PRIME: u64 = 0x0000_0100_0000_01b3;

/// `switch_refusal_key`: FNV-1a over the domain's bytes, one zero byte, then the reason's.
pub const fn key(domain: &str, reason: &str) -> u64 {
    let mut hash = fold(OFFSET, domain.as_bytes());
    hash = hash.wrapping_mul(PRIME);
    fold(hash, reason.as_bytes())
}

/// `switch_refusal_key_with`: the key continued over `detail`'s 8 bytes, least significant first.
pub const fn key_with(key: u64, detail: u64) -> u64 {
    fold(key, &detail.to_le_bytes())
}

const fn fold(mut hash: u64, bytes: &[u8]) -> u64 {
    let mut i = 0;
    while i < bytes.len() {
        hash = (hash ^ bytes[i] as u64).wrapping_mul(PRIME);
        i += 1;
    }
    hash
}

unsafe extern "C" {
    fn switch_refusal_first(key: u64) -> bool;
    fn switch_refusal_log(domain: *const c_char, reason_key: *const c_char, text: *const c_char);
}

/// One error line for `key` in this image, the first time it comes: `text` is made only then. The domain is a literal; the
/// reason, which the log keeps for the image's life, is copied and leaked once per line written.
pub fn refuse(domain: &'static CStr, reason: &str, key: u64, text: impl FnOnce() -> String) {
    // SAFETY: the C side takes any key, from any thread.
    if !unsafe { switch_refusal_first(key) } {
        return;
    }
    let reason = CString::new(reason).unwrap_or_default().into_raw();
    let text = CString::new(text().replace('\0', " ")).unwrap_or_default();
    // SAFETY: NUL-terminated strings; the domain and the leaked reason outlive the image's logging.
    unsafe { switch_refusal_log(domain.as_ptr(), reason, text.as_ptr()) };
}
