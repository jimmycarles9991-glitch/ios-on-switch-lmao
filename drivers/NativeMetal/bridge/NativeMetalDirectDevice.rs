// SPDX-License-Identifier: MIT
//! The direct compile on an m2v runtime device (the frontend's): the translator's compile devices taken from the
//! runtime's, its Vulkan device lending the layouts, and the runtime's own compute artifacts; and the compile profile
//! frozen from, and checked against, a live runtime. The device-free compile is native_metal_translate's.

use crate::compute_artifact::NmbComputeArtifact;
use crate::objects::{NmbComputePipelineDescriptor, NmbDevice, NmbRenderPipelineDescriptor, e, get, guard};
use metal2vulkan::Budget;
use metal2vulkan::cache::VariantKey;
use metal2vulkan::device::{Device, DeviceDescription, VkExtension, VkFeature};
use native_metal_translate::descriptors::{compute_descriptor, render_descriptor};
use native_metal_translate::direct::{Binds, NmbDirectBinds, NmbDirectState, RAW_HEAP_ELEMENTS, translate};
use native_metal_translate::direct_compute::{
    KernelVariants, NativeComputePipeline, NmbDirectDispatch, NmbDirectKernel, direct,
};
use native_metal_translate::offline::{Profile, frozen};
use std::ffi::{c_char, c_void};
use std::sync::{Arc, Mutex};

/// The compile device of the probe's barrier form: the runtime's enabled description without the pixel interlock, as the
/// pack's barrier probe ran NVK with it hidden (NVK_DEBUG=no_fsi). One per process, made at its first ask.
static BARRIER: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);

static ORDERED: Mutex<Option<(usize, Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);

static COMPUTE: Mutex<Option<([u8; 32], Arc<metal2vulkan::device::Device>)>> = Mutex::new(None);

pub fn native_compute_device(nmb: &NmbDevice) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = COMPUTE.lock().map_err(|_| "native compute device lock")?;
    let compile = nmb.device().compile_device();
    let fingerprint = compile.profile_fingerprint();
    if let Some((_, device)) = kept.as_ref().filter(|(of, _)| *of == fingerprint) {
        return Ok(device.clone());
    }
    let device = Arc::new(
        compile
            .with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53)
            .with_heap_elements(RAW_HEAP_ELEMENTS),
    );
    *kept = Some((fingerprint, device.clone()));
    Ok(device)
}

pub fn specialized_device(nmb: &NmbDevice, barrier: bool) -> Result<Arc<metal2vulkan::device::Device>, String> {
    let mut kept = if barrier { &BARRIER } else { &ORDERED }
        .lock()
        .map_err(|_| "native compile device lock")?;
    let id = nmb as *const NmbDevice as usize;
    if let Some((of, device)) = kept.as_ref().filter(|(of, _)| *of == id) {
        let _ = of;
        return Ok(device.clone());
    }
    let runtime = nmb.device();
    // SAFETY: the handles are the live runtime device's, used only for this query.
    let handles = unsafe { runtime.raw() };
    let description =
        unsafe { DeviceDescription::query(handles.instance, handles.physical_device, handles.instance_api_version) }
            .map_err(e("device description"))?;
    let mut enabled = description
        .with_enabled(runtime.enabled())
        .map_err(e("enabled description"))?;
    enabled.generated_commands = None;
    let mut policy = *runtime.compile_device().policy();
    let same = metal2vulkan::device::Device::new(enabled.clone(), policy).map_err(e("compile device"))?;
    if same.profile_fingerprint() != runtime.compile_device().profile_fingerprint() {
        return Err("the runtime's compile device is not its enabled description's".into());
    }
    if barrier {
        enabled.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderPixelInterlock);
        enabled.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    } else {
        policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::RequireOrdered;
        // The raw stage compiler explicitly interpolates FragCoord.z at the pixel center.
        // This profile uses that contract for sample-depth correction; the runtime profile stays unchanged.
    }
    let device = if barrier {
        metal2vulkan::device::Device::new(enabled, policy)
    } else {
        metal2vulkan::device::Device::new_with_center_fragment_depth(enabled, policy)
    };
    let device = Arc::new(
        device
            .map_err(e("native compile device"))?
            .with_native_target(metal2vulkan::device::NativeShaderTarget::Sm53)
            .with_heap_elements(RAW_HEAP_ELEMENTS),
    );
    *kept = Some((id, device.clone()));
    Ok(device)
}

/// `texture_format`, `texture_samples`: the probe's tagged textures' MTLPixelFormat (0: BGRA8Unorm) and sample count, as
/// probe.mm binds them for the state; `binds` (null: none): what it binds besides.
///
/// # Safety
/// `descriptor` is a valid record; `binds` is null or valid; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_direct(
    device: *mut NmbDevice,
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
        let binds = unsafe { Binds::of(binds) }?;
        let nmb = get(device)?;
        let descriptor = render_descriptor(descriptor)?;
        // Every raw form of a state (default, storage, ordered, barrier) and every compute kernel lowers half on SM53, so
        // a storage form and its barrier twin round alike. m2v's own twin stays on the Vulkan target (no float16 below SM70).
        let compile = if barrier || binds.fragment_tests != 0 {
            specialized_device(nmb, barrier)?
        } else {
            native_compute_device(nmb)?
        };
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { nmb.device().raw() };
        translate(
            &compile,
            &descriptor,
            Some(handles.device),
            texture_format,
            texture_samples,
            &binds,
            each,
            ctx,
        )
    })
}

/// # Safety
/// `descriptor` and `dispatch` are valid records; `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_direct(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let nmb = get(device)?;
        let native = native_compute_device(nmb)?;
        let descriptor = compute_descriptor(descriptor)?;
        let ask = get(dispatch)?;
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { nmb.device().raw() };
        direct(Some(handles.device), &native, &descriptor, None, ask, each, ctx)
    })
}

/// # Safety
/// The artifact and dispatch are live; the callback is valid for this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_artifact_direct(
    device: *mut NmbDevice,
    artifact: *const NmbComputeArtifact,
    dispatch: *const NmbDirectDispatch,
    each: unsafe extern "C" fn(kernel: *const NmbDirectKernel, ctx: *mut c_void) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let artifact = get(artifact)?;
        // SAFETY: the runtime device's own handles, used while it lives.
        let handles = unsafe { get(device)?.device().raw() };
        direct(
            Some(handles.device),
            &artifact.compile,
            &artifact.descriptor,
            Some(artifact as &dyn KernelVariants),
            get(dispatch)?,
            each,
            ctx,
        )
    })
}

/// The profile of `nmb`'s runtime: its queried description with what it enabled, as the runtime path's barrier and ordered
/// devices are made of it (direct.rs specialized_device)
fn live_profile(nmb: &NmbDevice) -> Result<Profile, String> {
    let runtime = nmb.device();
    // SAFETY: the handles are the live runtime device's, used only for this query.
    let handles = unsafe { runtime.raw() };
    let description =
        unsafe { DeviceDescription::query(handles.instance, handles.physical_device, handles.instance_api_version) }
            .map_err(e("device description"))?;
    let mut enabled = description
        .with_enabled(runtime.enabled())
        .map_err(e("enabled description"))?;
    enabled.generated_commands = None;
    Ok(Profile {
        enabled,
        policy: *runtime.compile_device().policy(),
    })
}

/// The compile profile of `device`'s runtime as JSON into `out` (`len` bytes, unterminated); its length, 0 on failure
///
/// # Safety
/// `out` is null or `len` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compile_profile_freeze(device: *mut NmbDevice, out: *mut c_char, len: usize) -> usize {
    guard(0, || {
        let json = serde_json::to_string_pretty(&live_profile(get(device)?)?).map_err(|e| e.to_string())?;
        if !out.is_null() && len >= json.len() {
            // SAFETY: the caller's contract.
            unsafe { std::ptr::copy_nonoverlapping(json.as_ptr(), out.cast(), json.len()) };
        }
        Ok(json.len())
    })
}

/// Whether the build's compile profile is `device`'s runtime's: its enabled description and policy, and the runtime's own
/// compile device the one made of them. False with nmb_last_error naming the difference.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compile_profile_check(device: *mut NmbDevice) -> bool {
    guard(false, || {
        let nmb = get(device)?;
        let live = live_profile(nmb)?;
        let frozen = frozen()?;
        if live.enabled != frozen.enabled {
            return Err("the runtime's enabled description is not the compile profile's".into());
        }
        if live.policy != frozen.policy {
            return Err("the runtime's compile policy is not the compile profile's".into());
        }
        let made = Device::new(frozen.enabled.clone(), frozen.policy).map_err(e("compile device"))?;
        if made.profile_fingerprint() != nmb.device().compile_device().profile_fingerprint() {
            return Err("the runtime's compile device is not the compile profile's".into());
        }
        Ok(true)
    })
}

impl KernelVariants for NmbComputeArtifact {
    fn pipeline(&self, key: &VariantKey, budget: &Budget) -> Result<Arc<NativeComputePipeline>, String> {
        NmbComputeArtifact::pipeline(self, key, budget)
    }
}
