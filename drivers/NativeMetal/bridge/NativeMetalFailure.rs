// SPDX-License-Identifier: MIT
//! NVK's switch_nvk_semaphore_failure as the runtime's submission failure hook: a command buffer whose work the kernel's
//! channel kill took (a GPU fault or hang, TegraGM20BABI.h method 11) fails with Metal's code, and its queue and device go
//! on, as Metal's do.

use metal2vulkan_runtime::Device;
use std::ffi::{CStr, c_void};
use std::sync::OnceLock;

type NvkFailure = unsafe extern "C" fn(semaphore: u64, value: u64) -> u32;

static NVK_FAILURE: OnceLock<NvkFailure> = OnceLock::new();

/// Method 11's error as `MTLCommandBufferError`: an MMU fault is a page fault (3), a hang a timeout (2), a GR exception
/// or a host error internal (1).
fn metal_code(kernel_error: u32) -> u32 {
    match kernel_error {
        0 => 0,
        1 => 3,
        4 => 2,
        _ => 1,
    }
}

unsafe extern "C" fn failure(semaphore: u64, value: u64) -> u32 {
    let Some(nvk) = NVK_FAILURE.get() else { return 0 };
    // SAFETY: NVK's export, thread-safe, alive while a device it made is.
    metal_code(unsafe { nvk(semaphore, value) })
}

/// The device asks NVK for each finished batch's failure, where the loaded NVK has the export.
pub(crate) fn tell_nvk_submission_failure(device: &Device, loader: &CStr) {
    // SAFETY: a NUL-terminated path; RTLD_NOLOAD loads nothing, and the device's open loaded NVK from this path.
    let nvk = unsafe { libc::dlopen(loader.as_ptr(), libc::RTLD_LAZY | libc::RTLD_NOLOAD) };
    if nvk.is_null() {
        return;
    }
    // SAFETY: a handle dlopen returned and a NUL-terminated name.
    let symbol = unsafe { libc::dlsym(nvk, c"switch_nvk_semaphore_failure".as_ptr()) };
    if !symbol.is_null() {
        // SAFETY: NVK exports it with NvkFailure's signature, and NVK is never unloaded while a device is open.
        let nvk_failure = unsafe { std::mem::transmute::<*mut c_void, NvkFailure>(symbol) };
        let _ = NVK_FAILURE.set(nvk_failure);
        // SAFETY: `failure` calls only NVK, which takes no lock of the runtime's.
        unsafe { device.set_submission_failure_hook(failure) };
    }
    // SAFETY: the reference RTLD_NOLOAD took.
    unsafe { libc::dlclose(nvk) };
}
