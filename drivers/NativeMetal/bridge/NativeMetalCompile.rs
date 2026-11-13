// SPDX-License-Identifier: MIT
//! The system shader compiler's compile context (NativeMetalCompile.h): the direct compile (NativeMetalDirect.rs,
//! NativeMetalDirectCompute.rs) with no runtime device, as the pack generator's offline compile makes it
//! (NativeMetalOffline.rs): on the build's compile profile's devices, with no Vulkan object, the variants' layouts
//! handed to NVK's stage compile as data. Its libraries and functions are the translator's own
//! (NativeMetalFunctions.rs) behind the NmbLibrary and NmbFunction handles.

use crate::direct::{NmbDirectBinds, NmbDirectState, native_compute_device, render_offline, specialized_device};
use crate::direct_compute::{NmbDirectDispatch, NmbDirectKernel, compute_offline};
use crate::functions::LibraryOf;
use crate::objects::{
    NmbComputePipelineDescriptor, NmbDevice, NmbLibrary, NmbRenderPipelineDescriptor, e, get, guard, library_handle,
};
use crate::offline::{Devices, nmb_compile_profile_check, offline_devices};
use std::ffi::{CStr, c_char, c_void};
use std::os::unix::ffi::OsStrExt;

pub struct NmbCompileContext {
    /// NVK, loaded as a runtime device loads it: the raw path finds NVK's stage compile in an image already loaded
    /// (NativeBridgeMetalFrontend.mm nvkSymbol)
    _nvk: ash::Entry,
    devices: &'static Devices,
}

fn open(loader: &CStr) -> Result<NmbCompileContext, String> {
    // SAFETY: NVK's image, which exports vkGetInstanceProcAddr, opened as a runtime device opens its loader.
    let nvk = unsafe { ash::Entry::load_from(std::ffi::OsStr::from_bytes(loader.to_bytes())) }.map_err(e("loader"))?;
    Ok(NmbCompileContext {
        _nvk: nvk,
        devices: offline_devices()?,
    })
}

/// # Safety
/// `loader` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_context_open(loader: *const c_char) -> *mut NmbCompileContext {
    guard(std::ptr::null_mut(), || {
        if loader.is_null() {
            return Err("a loader is required".into());
        }
        // SAFETY: the caller's contract.
        Ok(Box::into_raw(Box::new(open(unsafe { CStr::from_ptr(loader) })?)))
    })
}

/// A library of a metallib's AIR slice (`air`: of bare AIR) made without a device, as nmb_library_new makes one
///
/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_library_new(bytes: *const c_void, length: usize, air: bool) -> *mut NmbLibrary {
    guard(std::ptr::null_mut(), || {
        if bytes.is_null() || length == 0 {
            return Err("no library bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length) };
        let library = if air {
            metal2vulkan::library::Library::from_air(bytes).map_err(e("AIR"))?
        } else {
            metal2vulkan::library::Library::from_metallib(
                crate::air_slice(bytes).ok_or("metallib without an AIR64 slice")?,
            )
            .map_err(e("metallib"))?
        };
        Ok(library_handle(LibraryOf::Compile(library)))
    })
}

/// nmb_render_pipeline_offline on the context's functions
///
/// # Safety
/// As nmb_render_pipeline_direct's.
#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub unsafe extern "C" fn nmb_compile_render_direct(
    context: *const NmbCompileContext,
    descriptor: *const NmbRenderPipelineDescriptor,
    barrier: bool,
    texture_format: u64,
    texture_samples: u32,
    binds: *const NmbDirectBinds,
    each: unsafe extern "C" fn(state: *const NmbDirectState, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        // SAFETY: the caller's contract.
        unsafe {
            render_offline(
                get(context)?.devices,
                descriptor,
                barrier,
                texture_format,
                texture_samples,
                binds,
                each,
                ctx,
            )
        }
    })
}

/// nmb_compute_pipeline_offline on the context's functions
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_compute_direct(
    context: *const NmbCompileContext,
    descriptor: *const NmbComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        compute_offline(get(context)?.devices, descriptor, dispatch, each, ctx)
    })
}

/// Whether the context compiles as `device` does: the build's compile profile is its runtime's, and each compile device
/// a direct compile picks (native, barrier and ordered) is the one `device`'s picks
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_context_matches(context: *const NmbCompileContext, device: *const NmbDevice) -> bool {
    guard(false, || {
        let (devices, nmb) = (get(context)?.devices, get(device)?);
        if !nmb_compile_profile_check(device.cast_mut()) {
            return Ok(false);
        }
        let mut same = devices.native.profile_fingerprint() == native_compute_device(nmb)?.profile_fingerprint();
        for (offline, barrier) in [(&devices.barrier, true), (&devices.ordered, false)] {
            same &= offline.profile_fingerprint() == specialized_device(nmb, barrier)?.profile_fingerprint();
        }
        Ok(same)
    })
}
