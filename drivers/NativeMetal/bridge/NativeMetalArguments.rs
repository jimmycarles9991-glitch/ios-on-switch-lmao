// SPDX-License-Identifier: MIT
//! A runtime function's argument encoder layouts (translate/NativeMetalArgumentLayouts.rs's flat runs).

use crate::objects::{NmbFunction, e, get, guard};
use native_metal_translate::argument_layouts::{NmbArgumentLayout, with_flat_layouts};
use std::ffi::c_void;

/// `newArgumentEncoderWithBufferIndex:`'s layouts: the argument buffer `function` takes at buffer `index`, then each one its
/// pointers point to (with_flat_layouts), handed to `each`. False with nmb_last_error where Metal makes no encoder, or
/// `each`'s false.
///
/// # Safety
/// `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_argument_layouts(
    function: *mut NmbFunction,
    index: u32,
    each: unsafe extern "C" fn(layouts: *const NmbArgumentLayout, count: u32, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let encoder = get(function)?
            .function()?
            .new_argument_encoder_with_buffer_index(index)
            .map_err(e("argument encoder"))?;
        // SAFETY: the caller's contract; the layouts live until after the call.
        with_flat_layouts(encoder.layout(), |layouts| unsafe {
            each(layouts.as_ptr(), layouts.len() as u32, ctx)
        })
    })
}
