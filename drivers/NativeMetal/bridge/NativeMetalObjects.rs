// SPDX-License-Identifier: MIT
//! The bridge's Metal objects behind a C ABI (NativeMetalObjects.h): the metal2vulkan runtime's device, resources, states, queues,
//! command buffers and encoders as boxed handles for the Objective-C frontend. Raw Metal enumerations go through the runtime's own
//! checked conversions; a value it does not know fails the call (never a substitute). Failures leave their reason in a per-thread
//! string, and no call unwinds into C.

use crate::functions::{FunctionOf, LibraryOf};
use metal2vulkan::bind::{ArgumentDescriptor, SamplerDescriptor};
use metal2vulkan::library::{FunctionConstantValue, FunctionConstantValues};
use metal2vulkan::metal::state::{DepthStencilDescriptor, StencilDescriptor};
use metal2vulkan::metal::{
    AttributeFormat, BarrierScope, BindingAccess, BlendFactor, BlendOperation, ColorWriteMask, CompareFunction,
    CullMode, DataType, DepthClipMode, DispatchType, IndexType, PixelFormat, PrimitiveTopologyClass, PrimitiveType,
    RenderStages, ResourceUsage, SamplerAddressMode, SamplerBorderColor, SamplerMinMagFilter, SamplerMipFilter,
    StencilOperation, StepFunction, TessellationControlPointIndexType, TessellationFactorFormat,
    TessellationFactorStepFunction, TessellationPartitionMode, TextureType, TextureUsage, TriangleFillMode,
    VertexFormat, VertexStepFunction, Winding,
};
use metal2vulkan::pipeline::{
    ColorAttachmentDescriptor, ComputePipelineDescriptor, FunctionDescriptor, LinkedFunction, LinkedFunctions,
    RenderPipelineDescriptor, StageInputAttributeDescriptor, StageInputDescriptor, StageInputLayoutDescriptor,
    StageSource, TessellationDescriptor, VertexAttributeDescriptor, VertexBufferLayoutDescriptor, VertexDescriptor,
};
use metal2vulkan_runtime::{
    ArgumentEncoder, BinaryArchive, BinaryArchiveDescriptor, BlitCommandEncoder, BlitOption, Buffer, ClearColor,
    CommandBuffer, CommandQueue, ComputeCommandEncoder, ComputePipelineState, DebugMessage, DepthStencilState, Device,
    DeviceOptions, Event, EventRef, FamilyRow, FeatureRow, Fence, Function, GpuFamily, Heap, HeapDescriptor, HeapType,
    HostMemory, IoSurfacePlane, IoSurfaceYCbCr, LoadAction, MAX_COLOR_ATTACHMENTS, MessageSeverity, MessageSink,
    MetalLevel, MultisampleDepthResolveFilter, MultisampleStencilResolveFilter, Origin, PipelineOption,
    PipelineOptions, PrivateCount, PrivateFlag, PrivateProperties, PurgeableState, Region, RenderCommandEncoder,
    RenderPassDescriptor, RenderPipelineState, ResourceOptions, ResourceRef, RowState, SamplerState, ScissorRect,
    SharedEvent, SharedEventListener, Size, StoreAction, SubmissionMemoryHook, SurfaceYCbCrMatrix, Texture,
    TextureDescriptor, Viewport, VisibilityResultMode,
};
use std::cell::RefCell;
use std::ffi::{CStr, c_char, c_void};
use std::path::PathBuf;
use std::ptr::NonNull;
use std::time::Duration;

thread_local! {
    static LAST: RefCell<Vec<u8>> = const { RefCell::new(Vec::new()) };
}

fn set_error(why: String) {
    LAST.with(|last| {
        let mut last = last.borrow_mut();
        last.clear();
        last.extend(why.bytes().filter(|b| *b != 0));
        last.push(0);
    });
}

/// Runs `body`, turning an error or a panic into the per-thread reason and `fallback`.
pub(crate) fn guard<T>(fallback: T, body: impl FnOnce() -> Result<T, String>) -> T {
    match std::panic::catch_unwind(std::panic::AssertUnwindSafe(body)) {
        Ok(Ok(value)) => value,
        Ok(Err(why)) => {
            set_error(why);
            fallback
        }
        Err(_) => {
            set_error("panic in the Metal runtime".into());
            fallback
        }
    }
}

fn boxed<T>(value: T) -> *mut T {
    Box::into_raw(Box::new(value))
}

/// The object a handle points at.
pub(crate) fn get<'a, T>(handle: *const T) -> Result<&'a T, String> {
    // SAFETY: handles come from `boxed` and live until their release call (the header's contract).
    unsafe { handle.as_ref() }.ok_or_else(|| "null handle".to_string())
}

fn get_mut<'a, T>(handle: *mut T) -> Result<&'a mut T, String> {
    // SAFETY: as in `get`; an encoder is used by one thread at a time (Metal's rule).
    unsafe { handle.as_mut() }.ok_or_else(|| "null handle".to_string())
}

pub(crate) fn release<T>(handle: *mut T) {
    if !handle.is_null() {
        // SAFETY: from `boxed`, released once.
        drop(unsafe { Box::from_raw(handle) });
    }
}

fn copy_out(text: &str, out: *mut c_char, size: usize) -> usize {
    if !out.is_null() && size > 0 {
        let n = text.len().min(size - 1);
        // SAFETY: the caller's buffer holds `size` bytes.
        unsafe {
            std::ptr::copy_nonoverlapping(text.as_ptr(), out as *mut u8, n);
            *out.add(n) = 0;
        }
    }
    text.len()
}

pub(crate) fn raw<T: TryFrom<u32>>(value: u64, what: &str) -> Result<T, String>
where
    T::Error: std::fmt::Debug,
{
    u32::try_from(value)
        .ok()
        .and_then(|v| T::try_from(v).ok())
        .ok_or_else(|| format!("{what} {value} is not a Metal value"))
}

/// The error of `what` as the caller's text, and m2v's refusal of work iPadOS sent, logged once per call and kind of error
/// in the process (kits/SwitchRefusal: an error line, kept in release). The kind is the error's variant and first field
/// (`NotImplemented(Format`, `Unsupported(Unsupported { feature: FramebufferFetch`) without digits, so a value that varies
/// per call makes no line of its own.
pub(crate) fn e<E: std::fmt::Debug>(what: &'static str) -> impl FnOnce(E) -> String {
    move |error| {
        let text = format!("{what}: {error:?}");
        let kind: String = text[what.len() + 2..]
            .split([',', ')'])
            .next()
            .unwrap_or_default()
            .chars()
            .filter(|c| !c.is_ascii_digit())
            .collect();
        let key = switch_refusal::key_with(switch_refusal::key("m2v", what), switch_refusal::key("", &kind));
        switch_refusal::refuse(c"m2v", what, key, || text.clone());
        text
    }
}

fn options(value: u64) -> Result<ResourceOptions, String> {
    ResourceOptions::from_raw(value).map_err(e("resource options"))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_last_error() -> *const c_char {
    LAST.with(|last| {
        let last = last.borrow();
        if last.is_empty() {
            c"".as_ptr()
        } else {
            last.as_ptr() as *const c_char
        }
    })
}

/// The runtime's kernel_function_constant_gate_fold fixture, assembled to AIR at build time.
static FUNCTION_CONSTANT_AIR: &[u8] = include_bytes!(env!("NMB_FC_AIR"));

/// # Safety
/// `length` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_fixture_air(length: *mut usize) -> *const c_void {
    // SAFETY: the caller's contract.
    if let Some(length) = unsafe { length.as_mut() } {
        *length = crate::FIXTURE_AIR.len();
    }
    crate::FIXTURE_AIR.as_ptr() as *const c_void
}

/// # Safety
/// `length` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_constant_air(length: *mut usize) -> *const c_void {
    // SAFETY: the caller's contract.
    if let Some(length) = unsafe { length.as_mut() } {
        *length = FUNCTION_CONSTANT_AIR.len();
    }
    FUNCTION_CONSTANT_AIR.as_ptr() as *const c_void
}

// ---- device ----

pub struct NmbDevice(Device, std::sync::OnceLock<PrivateProperties>, std::sync::Mutex<()>);

impl NmbDevice {
    pub(crate) fn device(&self) -> &Device {
        &self.0
    }

    /// The runtime judges the private properties by the family rows once.
    fn private(&self) -> &PrivateProperties {
        self.1.get_or_init(|| self.0.private_properties())
    }
}

/// Whether the environment variable `name` is set to something other than 0.
fn env_on(name: &str, otherwise: bool) -> bool {
    std::env::var(name).map_or(otherwise, |value| value != "0")
}

/// # Safety
/// `loader` is a C string; `cache` a C string or null. A null `cache` keeps no shader cache on disk: the runtime's memo and the
/// shared seed alone (METAL2VULKAN_SHADER_CACHE_SEED_ONLY, m2v patch 0110: no directory is made, read or written, so a sandbox
/// that refuses every directory never sees a write tried): for a process that can write no directory.
/// The environment: NMB_MEMO_MB caps the memo (MiB; the runtime's 256 by default); NMB_REFUSE_REBINDS=0 lets encoders log
/// their calls for rebinds again (nothing here rebinds); NMB_PADDED_ALIAS=1 renders and writes a texture over padded rows in
/// place (PaddedReadsPolicy::AliasWrites, off: untried on NVK). Every texture given a shadow image is written to
/// <cache>/shadow.log with why, once per reason, unless NMB_SHADOW_LOG=0 (a release plugin's). NMB_CACHE_MB caps the cache
/// directory (MiB, 256 by default).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_device_open(loader: *const c_char, cache: *const c_char) -> *mut NmbDevice {
    guard(std::ptr::null_mut(), || {
        if loader.is_null() {
            return Err("a loader is required".into());
        }
        // SAFETY: the caller's contract.
        let loader = unsafe { CStr::from_ptr(loader) };
        let cache = if cache.is_null() {
            // SAFETY: as the plugin's own setenv calls before the device opens, under its lock; nothing reads this name meanwhile.
            unsafe { std::env::set_var("METAL2VULKAN_SHADER_CACHE_SEED_ONLY", "1") };
            None
        } else {
            // SAFETY: the caller's contract.
            Some(PathBuf::from(
                unsafe { CStr::from_ptr(cache) }.to_string_lossy().into_owned(),
            ))
        };
        let mut options = DeviceOptions::default();
        options.policy.framebuffer_fetch = metal2vulkan::device::FramebufferFetchPolicy::AcceptUnordered;
        options.loader = Some(PathBuf::from(loader.to_string_lossy().into_owned()));
        options.metal_level = Some(MetalLevel::NEWEST);
        options.shader_cache = metal2vulkan_runtime::ShaderCacheOptions::in_directory(
            cache.clone().unwrap_or_else(|| PathBuf::from("/private/var/empty/m2v")),
        );
        if let Some(mib) = std::env::var("NMB_MEMO_MB")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .filter(|&mib| mib > 0)
        {
            options.shader_cache.memory_capacity = mib << 20;
        }
        // The directory's cap: 256 MiB unless NMB_CACHE_MB says (the runtime's 25 GiB would let the Data volume fill).
        let cap = std::env::var("NMB_CACHE_MB")
            .ok()
            .and_then(|v| v.parse::<u64>().ok())
            .filter(|&mib| mib > 0)
            .unwrap_or(256);
        options.shader_cache.capacity = cap << 20;
        options.refuse_rebinds = env_on("NMB_REFUSE_REBINDS", true);
        if env_on("NMB_PADDED_ALIAS", false) {
            options.policy.padded_reads = metal2vulkan::device::PaddedReadsPolicy::AliasWrites;
        }
        if let Some(cache) = cache.as_ref().filter(|_| env_on("NMB_SHADOW_LOG", true)) {
            options
                .announce_each
                .insert(metal2vulkan_runtime::Degradation::ShadowedTexture);
            let _ = SHADOW_LOG.set(cache.join("shadow.log"));
        }
        options.messages = Some(MessageSink::new(observe_message));
        let device = Device::system_default_with(&options).map_err(e("device"))?;
        tell_nvk_submission_memory(&device, loader);
        crate::failure::tell_nvk_submission_failure(&device, loader);
        Ok(boxed(NmbDevice(
            device,
            std::sync::OnceLock::new(),
            std::sync::Mutex::new(()),
        )))
    })
}

/// NVK's switch_nvk_submit_surfaces, where the loaded NVK has it: each submission's host memory, so the kernel orders other
/// processes' GPU work on those imports (ExecSurfaces) and CPU locks wait only for writers.
fn tell_nvk_submission_memory(device: &Device, loader: &CStr) {
    // SAFETY: a NUL-terminated path; RTLD_NOLOAD loads nothing, and the device's open loaded NVK from this path.
    let nvk = unsafe { libc::dlopen(loader.as_ptr(), libc::RTLD_LAZY | libc::RTLD_NOLOAD) };
    if nvk.is_null() {
        return;
    }
    // SAFETY: a handle dlopen returned and a NUL-terminated name.
    let hook = unsafe { libc::dlsym(nvk, c"switch_nvk_submit_surfaces".as_ptr()) };
    if !hook.is_null() {
        // SAFETY: NVK exports it with SubmissionMemoryHook's signature; it is thread-safe, lives as long as NVK (never
        // unloaded while a device is open) and calls nothing of the runtime's.
        unsafe {
            device.set_submission_memory_hook(std::mem::transmute::<*mut c_void, SubmissionMemoryHook>(hook));
        }
    }
    // SAFETY: the reference RTLD_NOLOAD took.
    unsafe { libc::dlclose(nvk) };
}

// What the runtime says through the device's sink, kept as numbers for the plugin's telemetry: each render VkPipeline making the
// creation log (METAL2VULKAN_LOG_PIPELINES) reports ("... made in <ms> ms[ after waiting <ms> ms for its turn]"), and the other
// messages by severity with the last warning's text.
static PIPELINE_MAKINGS: [std::sync::atomic::AtomicU64; 5] = [const { std::sync::atomic::AtomicU64::new(0) }; 5]; // n, us, max us, waited us, failed
static OTHER_MESSAGES: [std::sync::atomic::AtomicU64; 2] = [const { std::sync::atomic::AtomicU64::new(0) }; 2]; // info, warning or worse
static LAST_WARNING: std::sync::Mutex<String> = std::sync::Mutex::new(String::new());

fn millis_after(text: &str, marker: &str) -> Option<f64> {
    let at = text.find(marker)? + marker.len();
    let rest = &text[at..];
    let end = rest.find(' ').unwrap_or(rest.len());
    rest[..end].parse().ok()
}

// The distinct reasons textures were given shadow images, their numbers masked, each written once to the cache directory's
// shadow.log with the first such texture's message, at most SHADOW_REASONS of them (AdrenoMetal's bridge).
static SHADOW_LOG: std::sync::OnceLock<PathBuf> = std::sync::OnceLock::new();
static SHADOW_SEEN: std::sync::Mutex<Vec<String>> = std::sync::Mutex::new(Vec::new());
static SHADOWS: std::sync::atomic::AtomicU64 = std::sync::atomic::AtomicU64::new(0);
const SHADOW_REASONS: usize = 64;

fn note_shadow(text: &str) {
    SHADOWS.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
    let masked: String = text.chars().map(|c| if c.is_ascii_digit() { '#' } else { c }).collect();
    let Ok(mut seen) = SHADOW_SEEN.lock() else { return };
    if seen.len() >= SHADOW_REASONS || seen.contains(&masked) {
        return;
    }
    seen.push(masked);
    if let Some(path) = SHADOW_LOG.get() {
        use std::io::Write;
        if let Ok(mut file) = std::fs::OpenOptions::new().create(true).append(true).open(path) {
            let _ = writeln!(file, "{text}");
        }
    }
}

fn observe_message(message: &DebugMessage) {
    use std::sync::atomic::Ordering::Relaxed;
    if message.id_name == "shadowed-texture" {
        note_shadow(&message.text);
    }
    if message.id_name == "metal2vulkan-pipeline-creation" {
        let made = millis_after(&message.text, " made in ");
        let us = made
            .or_else(|| millis_after(&message.text, " failed in "))
            .map_or(0, |ms| (ms * 1000.0) as u64);
        PIPELINE_MAKINGS[0].fetch_add(1, Relaxed);
        PIPELINE_MAKINGS[1].fetch_add(us, Relaxed);
        PIPELINE_MAKINGS[2].fetch_max(us, Relaxed);
        PIPELINE_MAKINGS[3].fetch_add(
            millis_after(&message.text, " after waiting ").map_or(0, |ms| (ms * 1000.0) as u64),
            Relaxed,
        );
        if made.is_none() {
            PIPELINE_MAKINGS[4].fetch_add(1, Relaxed);
        }
        return;
    }
    if message.severity >= MessageSeverity::Warning {
        OTHER_MESSAGES[1].fetch_add(1, Relaxed);
        if let Ok(mut last) = LAST_WARNING.lock() {
            *last = message.text.chars().take(160).collect();
        }
    } else {
        OTHER_MESSAGES[0].fetch_add(1, Relaxed);
    }
}

/// The sink's counts into the caller's ten words: pipeline makings (count, total us, longest us, waited us, failed), messages (info,
/// warning or worse), and the device's shader cache hits, misses and writes.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_work_stats(device: *const NmbDevice, out: *mut u64) -> bool {
    use std::sync::atomic::Ordering::Relaxed;
    guard(false, || {
        if out.is_null() {
            return Err("null stats".into());
        }
        // SAFETY: the header's ten words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 10) };
        for (i, v) in PIPELINE_MAKINGS.iter().enumerate() {
            out[i] = v.load(Relaxed);
        }
        out[5] = OTHER_MESSAGES[0].load(Relaxed);
        out[6] = OTHER_MESSAGES[1].load(Relaxed);
        let cache = get(device)?.0.shader_cache_statistics();
        out[7] = cache.hits;
        out[8] = cache.misses;
        out[9] = cache.writes;
        Ok(true)
    })
}

/// The runtime's own counts into the caller's ten words: device memory allocated (bytes), pooled, dedicated and imported blocks
/// live, timeline waits (CPU waits for the GPU), render variants' VkPipelines made, those linked from libraries, fragment
/// shader libraries made, textures given shadow images, and dedicated requests a spare block served (the runtime's ios-on-switch-lmao
/// spare dedicated blocks, `memory.rs`).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_counts(device: *const NmbDevice, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null counts".into());
        }
        let c = get(device)?.0.work_counts();
        // SAFETY: the header's ten words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 10) };
        out.copy_from_slice(&[
            c.device_memory_allocated,
            c.memory_blocks_live,
            c.dedicated_memory_blocks_live,
            c.host_imports_live,
            c.timeline_waits,
            c.variant_pipelines_made,
            c.pipelines_linked,
            c.fragment_shader_libraries_made,
            SHADOWS.load(std::sync::atomic::Ordering::Relaxed),
            c.spare_block_reuses,
        ]);
        Ok(true)
    })
}

/// The runtime's device memory into the caller's three words: the bytes it holds now (its live blocks and spares), the
/// bytes it allocated so far, and the memoryless backings live (`memory.rs`).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_memory(device: *const NmbDevice, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null memory".into());
        }
        let c = get(device)?.0.work_counts();
        // SAFETY: the header's three words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 3) };
        out.copy_from_slice(&[
            c.device_memory_live,
            c.device_memory_allocated,
            c.memoryless_backings_live,
        ]);
        Ok(true)
    })
}

/// Free the device memory no resource holds (`Device::release_idle_memory`): the bytes freed.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_release_idle_memory(device: *const NmbDevice) -> u64 {
    guard(0, || Ok(get(device)?.0.release_idle_memory()))
}

/// The runtime's host texel copies by route, into the caller's seven words (ios-on-switch-lmao m2v 0116): `replaceRegion` into the texture's
/// bytes on the CPU, on the host into its image, staged for the next submission, by a submission waited for; then `getBytes` from
/// the texture's bytes, on the host from its image, by a submission waited for.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_texel_copy_routes(device: *const NmbDevice, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null routes".into());
        }
        let c = get(device)?.0.work_counts();
        // SAFETY: the header's seven words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 7) };
        out[..4].copy_from_slice(&c.replace_region_routes);
        out[4..].copy_from_slice(&c.get_bytes_routes);
        Ok(true)
    })
}

/// The runtime's host calls that waited for the GPU, into the caller's four words: submissions on the staging queue
/// (`replace_region`, `get_bytes` and first moves of textures it copies none of on the host), internal submissions (a host
/// copy through the GPU, a clear of a live buffer, a neutral's making), `replace_region`/`get_bytes` copies made on the host
/// with no submission, and the queue submissions of commits.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_wait_counts(device: *const NmbDevice, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null counts".into());
        }
        let c = get(device)?.0.work_counts();
        // SAFETY: the header's four words.
        let out = unsafe { std::slice::from_raw_parts_mut(out, 4) };
        out.copy_from_slice(&[
            c.staging_submissions,
            c.internal_submissions,
            c.host_texel_copies,
            c.queue_submissions,
        ]);
        Ok(true)
    })
}

/// Separate artifact and driver-record counters, mirroring the C ABI.
#[repr(C)]
pub struct NmbShaderCacheStats {
    pub hits: u64,
    pub misses: u64,
    pub rejected: u64,
    pub read_failures: u64,
    pub writes: u64,
    pub write_failures: u64,
    pub evictions: u64,
    pub driver_loads: u64,
    pub driver_hits: u64,
    pub driver_misses: u64,
    pub driver_saves: u64,
    pub driver_truncated: u64,
    pub driver_mismatched: u64,
    pub driver_unchanged: u64,
    pub seed_misses: u64,
    pub seed_hit_p50_ns: u64,
    pub seed_hit_p99_ns: u64,
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_shader_cache_stats(device: *const NmbDevice, out: *mut NmbShaderCacheStats) -> bool {
    guard(false, || {
        let out = get_mut(out)?;
        let stats = get(device)?.0.shader_cache_statistics();
        *out = NmbShaderCacheStats {
            hits: stats.hits,
            misses: stats.misses,
            rejected: stats.rejected,
            read_failures: stats.read_failures,
            writes: stats.writes,
            write_failures: stats.write_failures,
            evictions: stats.evictions,
            driver_loads: stats.driver_loads,
            driver_hits: stats.driver_pipeline_hits,
            driver_misses: stats.driver_pipeline_misses,
            driver_saves: stats.driver_saves,
            driver_truncated: stats.driver_data_truncated,
            driver_mismatched: stats.driver_data_mismatched,
            driver_unchanged: stats.driver_data_unchanged,
            seed_misses: stats.seed_misses,
            seed_hit_p50_ns: stats.seed_hit_p50_ns,
            seed_hit_p99_ns: stats.seed_hit_p99_ns,
        };
        Ok(true)
    })
}

/// The last warning the device's sink received, as a C string; returns its length.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_last_warning(out: *mut c_char, size: usize) -> usize {
    guard(0, || {
        Ok(copy_out(
            &LAST_WARNING.lock().map(|s| s.clone()).unwrap_or_default(),
            out,
            size,
        ))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_release(device: *mut NmbDevice) {
    release(device)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_name(device: *const NmbDevice, out: *mut c_char, size: usize) -> usize {
    guard(0, || Ok(copy_out(get(device)?.0.name(), out, size)))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_max_buffer_length(device: *const NmbDevice) -> u64 {
    guard(0, || Ok(get(device)?.0.properties().max_buffer_length))
}

/// `maxThreadsPerThreadgroup` into the caller's three words.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_max_threads_per_threadgroup(device: *const NmbDevice, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null size".into());
        }
        let s = get(device)?.0.properties().max_threads_per_threadgroup;
        // SAFETY: the header's three words.
        unsafe { std::slice::from_raw_parts_mut(out, 3) }.copy_from_slice(&[s.width, s.height, s.depth]);
        Ok(true)
    })
}

/// The device selectors `nmb_device_answer` takes beyond Apple's private counts and flags: the selector and its kind
/// (the header's NMB_ANSWER_* values).
const PUBLIC_ANSWERS: &[(&str, u32)] = &[
    ("maxThreadgroupMemoryLength", 0),
    ("argumentBuffersSupport", 0),
    ("readWriteTextureSupport", 0),
    ("maxColorAttachments", 0),
    ("minConstantBufferAlignmentBytes", 0),
    ("deviceLinearReadOnlyTextureAlignmentBytes", 0),
    ("areRasterOrderGroupsSupported", 1),
    ("supportsRasterOrderGroups", 1),
    ("isDepth24Stencil8PixelFormatSupported", 1),
    ("areProgrammableSamplePositionsSupported", 1),
    ("supports32BitFloatFiltering", 1),
    ("supportsMemorylessRenderTargets", 1),
    ("supportsSamplerReductionMode", 1),
    ("supportsYCBCRFormats12", 1),
    ("supportsYCBCRPackedFormats12", 1),
    ("supportsSIMDGroup", 1),
    ("minimumLinearTextureAlignmentForPixelFormat:", 2),
    ("supportsTextureSampleCount:", 3),
    ("supportsVertexAmplificationCount:", 3),
    ("supportsRasterizationRateMapWithLayerCount:", 3),
    ("supportsFamily:", 3),
    ("supportsPrimitiveType:", 3),
    ("supportsTextureWriteRoundingMode:", 3),
];

/// Every selector `nmb_device_answer` takes, with its kind, as C strings.
fn answer_selectors() -> &'static [(std::ffi::CString, u32)] {
    static ALL: std::sync::OnceLock<Vec<(std::ffi::CString, u32)>> = std::sync::OnceLock::new();
    ALL.get_or_init(|| {
        let counts = PrivateCount::ALL.into_iter().map(|c| (c.selector(), 0));
        let flags = PrivateFlag::ALL.into_iter().map(|f| (f.selector(), 1));
        let mut all: Vec<(&str, u32)> = counts.chain(flags).chain(PUBLIC_ANSWERS.iter().copied()).collect();
        for selector in crate::target::count_selectors() {
            if !all.iter().any(|&(s, _)| s == selector) {
                all.push((selector, 0));
            }
        }
        all.into_iter()
            .map(|(s, kind)| (std::ffi::CString::new(s).expect("selector"), kind))
            .collect()
    })
}

/// The selector at `index` of the ones `nmb_device_answer` takes and its kind; null past the last.
///
/// # Safety
/// `kind` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_device_answer_selector(index: usize, kind: *mut u32) -> *const c_char {
    let Some((selector, k)) = answer_selectors().get(index) else {
        return std::ptr::null();
    };
    // SAFETY: the caller's contract.
    if let Some(kind) = unsafe { kind.as_mut() } {
        *kind = *k;
    }
    selector.as_ptr()
}

/// The rows of Apple's tables the device does not meet in the target families `supportsFamily:` answers true (`target::FAMILIES`), one
/// family per line ("Apple2 4: <row> ..."), into `out` (NUL-terminated); the length the whole text needs.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_family_waivers(device: *const NmbDevice, out: *mut c_char, size: usize) -> usize {
    guard(0, || {
        let d = &get(device)?.0;
        let mut text = String::new();
        for &family in crate::target::FAMILIES {
            let blockers = d.family_blockers(family);
            text += &format!("{family:?} {}:", blockers.len());
            for r in &blockers {
                text += &format!(" {}", r.row.label());
                if let FamilyRow::Format(_, capability) = r.row {
                    text += &format!("/{capability:?}");
                }
                text += ";";
            }
            text.push('\n');
        }
        if !out.is_null() && size > 0 {
            let n = text.len().min(size - 1);
            // SAFETY: `out` holds `size` bytes (the caller's contract).
            unsafe {
                std::ptr::copy_nonoverlapping(text.as_ptr(), out.cast::<u8>(), n);
                *out.add(n) = 0;
            }
        }
        Ok(text.len())
    })
}

/// The device's answer: the runtime's, never above the target hardware's (NativeMetalTarget.rs).
fn device_answer(device: &NmbDevice, selector: &str, arg: u64) -> Result<u64, String> {
    if crate::target::says_no(selector) {
        return Ok(0);
    }
    let own = runtime_answer(device, selector, arg);
    let Some(target) = crate::target::count(selector) else {
        return own;
    };
    let own = own.or_else(|reason| {
        let color_attachments = runtime_answer(device, "maxColorAttachments", 0)?;
        crate::target::device_bound(&device.0, selector, color_attachments).ok_or(reason)
    })?;
    Ok(own.min(target))
}

fn runtime_answer(device: &NmbDevice, selector: &str, arg: u64) -> Result<u64, String> {
    let d = &device.0;
    let p = d.properties();
    let none = || format!("the device has no answer for {selector}");
    if let Some(c) = PrivateCount::ALL.into_iter().find(|c| c.selector() == selector) {
        return device.private().count(c).value().ok_or_else(none);
    }
    if let Some(f) = PrivateFlag::ALL.into_iter().find(|f| f.selector() == selector) {
        return device.private().flag(f).value().map(u64::from).ok_or_else(none);
    }
    let gap =
        |r: Result<bool, metal2vulkan_runtime::RuntimeGap>| r.map(u64::from).map_err(|g| format!("{selector}: {g:?}"));
    let format_supported =
        |f: PixelFormat| d.pixel_format_capabilities(f) != metal2vulkan_runtime::FormatCapabilities::NONE;
    Ok(match selector {
        "maxThreadgroupMemoryLength" => p.max_threadgroup_memory_length,
        "argumentBuffersSupport" => p.argument_buffers_support.as_raw() as u64,
        "readWriteTextureSupport" => p.read_write_texture_support.as_raw() as u64,
        // The runtime's pass takes MAX_COLOR_ATTACHMENTS; the device's Vulkan limits bound what a pipeline links.
        "maxColorAttachments" => {
            let l = &p.description.limits;
            (MAX_COLOR_ATTACHMENTS as u64).min(l.max_color_attachments.min(l.max_fragment_output_attachments) as u64)
        }
        // The runtime binds a buffer at any offset within it (render.rs, setVertexBuffer); 4 is a scalar's alignment, which the shader's
        // own loads need.
        "minConstantBufferAlignmentBytes" => 4,
        "deviceLinearReadOnlyTextureAlignmentBytes" => device
            .private()
            .count(PrivateCount::DeviceLinearTextureAlignmentBytes)
            .value()
            .ok_or_else(none)?,
        "areRasterOrderGroupsSupported" | "supportsRasterOrderGroups" => gap(p.are_raster_order_groups_supported())?,
        "isDepth24Stencil8PixelFormatSupported" => gap(p.is_depth24_stencil8_pixel_format_supported())?,
        "areProgrammableSamplePositionsSupported" => gap(p.are_programmable_sample_positions_supported())?,
        "supports32BitFloatFiltering" => p.supports_32bit_float_filtering as u64,
        // A Memoryless texture is a transient render attachment (texture.rs), which every device makes.
        "supportsMemorylessRenderTargets" => 1,
        "supportsSamplerReductionMode" => gap(p.supports_sampler_reduction_mode())?,
        // Apple's SIMD-group functions: the family's SIMD-scoped barrier and permute rows are exact on the device.
        "supportsSIMDGroup" => {
            let rows = d.family_rows(GpuFamily::Apple9);
            let exact = |r: FeatureRow| {
                rows.iter()
                    .any(|a| a.row == FamilyRow::Feature(r) && a.state == RowState::Exact)
            };
            (exact(FeatureRow::SimdBarrier) && exact(FeatureRow::SimdPermute)) as u64
        }
        "supportsYCBCRFormats12" => format_supported(PixelFormat::YCBCR124202P) as u64,
        "supportsYCBCRPackedFormats12" => format_supported(PixelFormat::YCBCR124202PPACKED) as u64,
        "minimumLinearTextureAlignmentForPixelFormat:" => p
            .minimum_linear_texture_alignment_for_pixel_format(raw::<PixelFormat>(arg, "pixel format")?)
            .map_err(e("minimum linear texture alignment"))?,
        "supportsTextureSampleCount:" => gap(p.supports_texture_sample_count(arg))?,
        "supportsVertexAmplificationCount:" => gap(p.supports_vertex_amplification_count(arg))?,
        "supportsRasterizationRateMapWithLayerCount:" => gap(p.supports_rasterization_rate_map_with_layer_count(arg))?,
        // The target's families and no others: a family the runtime meets that the target lacks (Metal3 on either target)
        // would send the OS down paths no target device takes.
        "supportsFamily:" => crate::target::FAMILIES.iter().any(|f| f.raw() == arg as i64) as u64,
        "supportsPrimitiveType:" => p.supports_primitive_type(arg) as u64,
        "supportsTextureWriteRoundingMode:" => p.supports_texture_write_rounding_mode(arg) as u64,
        _ => return Err(format!("{selector} is not a device query of the bridge")),
    })
}

/// The runtime's answer on the device to `selector` (one `nmb_device_answer_selector` lists), with `arg` for a selector that takes one.
/// False, with the reason, where the device has no answer of its own.
///
/// # Safety
/// `selector` is a C string; `out` is writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_device_answer(
    device: *const NmbDevice,
    selector: *const c_char,
    arg: u64,
    out: *mut u64,
) -> bool {
    guard(false, || {
        if selector.is_null() || out.is_null() {
            return Err("null selector or answer".into());
        }
        // SAFETY: the caller's contract.
        let selector = unsafe { CStr::from_ptr(selector) }.to_str().map_err(e("selector"))?;
        let answer = device_answer(get(device)?, selector, arg)?;
        // SAFETY: as above.
        unsafe { *out = answer };
        Ok(true)
    })
}

/// `heapBufferSizeAndAlignWithLength:options:` into `out` (size, alignment).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_heap_buffer_size(
    device: *const NmbDevice,
    length: u64,
    options_word: u64,
    out: *mut u64,
) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null size".into());
        }
        let answer = get(device)?
            .0
            .heap_buffer_size_and_align(length, options(options_word)?);
        // SAFETY: the header's two words.
        unsafe { std::slice::from_raw_parts_mut(out, 2) }.copy_from_slice(&[answer.size, answer.align]);
        Ok(true)
    })
}

/// `heapTextureSizeAndAlignWithDescriptor:` into `out` (size, alignment); false, with the reason, for a descriptor refused.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_heap_texture_size(
    device: *const NmbDevice,
    descriptor: *const NmbTextureDescriptor,
    out: *mut u64,
) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null size".into());
        }
        let answer = get(device)?
            .0
            .heap_texture_size_and_align(&texture_descriptor(descriptor)?)
            .map_err(e("heap texture size"))?;
        // SAFETY: the header's two words.
        unsafe { std::slice::from_raw_parts_mut(out, 2) }.copy_from_slice(&[answer.size, answer.align]);
        Ok(true)
    })
}

/// `requiredLinearTextureBytesPerRowForDescriptor:` (Apple SPI): a row of `width` texels of `format` rounded up to the format's
/// linear alignment, from the runtime's format description; 0, with the reason, for a format no buffer can back.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_linear_bytes_per_row(device: *const NmbDevice, format: u64, width: u64) -> u64 {
    guard(0, || {
        let d = &get(device)?.0;
        let alignment = d
            .properties()
            .minimum_linear_texture_alignment_for_pixel_format(raw::<PixelFormat>(format, "pixel format")?)
            .map_err(e("linear texture"))?;
        let info = d.pixel_format_info(format);
        // Flags bit 0x400: a format of blocks, which the alignment query has refused already.
        if info.flags & 0x400 != 0 || info.words[0] == 0 {
            return Err(format!("pixel format {format} has no texel size"));
        }
        Ok((width * info.words[0]).div_ceil(alignment) * alignment)
    })
}

/// A format's block as Metal describes it (`MTLPixelFormatGetInfoForDevice`): `out` = its bytes, width, height and depth,
/// a texel being a 1x1x1 block of a format not made of blocks. False, with the reason, for a format Metal does not describe.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_pixel_format_block(device: *const NmbDevice, format: u64, out: *mut u64) -> bool {
    guard(false, || {
        if out.is_null() {
            return Err("null block".into());
        }
        let info = get(device)?.0.pixel_format_info(format);
        if info.words[0] == 0 {
            return Err(format!("pixel format {format} has no texel or block size"));
        }
        // Flags bit 0x400: a format of blocks, whose words are the block's.
        let block = if info.flags & 0x400 != 0 {
            info.words
        } else {
            [info.words[0], 1, 1, 1]
        };
        if block[1..].contains(&0) {
            return Err(format!("pixel format {format} has an empty block"));
        }
        // SAFETY: the header's four words.
        unsafe { std::slice::from_raw_parts_mut(out, 4) }.copy_from_slice(&block);
        Ok(true)
    })
}

/// `vendorName`: the Vulkan device's PCI vendor, named where the ID is a known GPU vendor's, else the ID itself.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_device_vendor_name(device: *const NmbDevice, out: *mut c_char, size: usize) -> usize {
    guard(0, || {
        let id = get(device)?.0.properties().vendor_id;
        let name = match id {
            0x5143 => "Qualcomm".to_string(),
            0x106b => "Apple".to_string(),
            0x10de => "NVIDIA".to_string(),
            0x1002 => "AMD".to_string(),
            0x8086 => "Intel".to_string(),
            0x13b5 => "ARM".to_string(),
            _ => format!("0x{id:04x}"),
        };
        Ok(copy_out(&name, out, size))
    })
}

// ---- buffers ----

pub struct NmbBuffer(Buffer);

/// # Safety
/// `bytes` is null or holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_buffer_new(
    device: *mut NmbDevice,
    length: u64,
    options_word: u64,
    bytes: *const c_void,
) -> *mut NmbBuffer {
    guard(std::ptr::null_mut(), || {
        let device = &get(device)?.0;
        let options = options(options_word)?;
        let buffer = if bytes.is_null() {
            device.new_buffer(length, options)
        } else {
            // SAFETY: the caller's contract.
            let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length as usize) };
            device.new_buffer_with_bytes(bytes, options)
        };
        Ok(boxed(NmbBuffer(buffer.map_err(e("buffer"))?)))
    })
}

/// # Safety
/// `pointer` is page aligned, holds `length` bytes and stays mapped until the buffer is released.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_buffer_new_no_copy(
    device: *mut NmbDevice,
    pointer: *mut c_void,
    length: u64,
    options_word: u64,
) -> *mut NmbBuffer {
    guard(std::ptr::null_mut(), || {
        let device = get(device)?;
        // SAFETY: the caller's contract.
        let memory = unsafe { HostMemory::new(NonNull::new(pointer as *mut u8).ok_or("null memory")?) };
        let modes = options(options_word)?;
        // The runtime's live-import lookup and subsequent join are separate:
        // simultaneous first constructors can allocate distinct imports of the
        // same pages. Cover every native ABI no-copy call, including mixtures
        // of raw buffers and IOSurface textures. Each call still gets its own
        // buffer object. This ABI passes neither a dma-buf source nor a runtime
        // deallocator; no caller callback runs under this gate. Drop it before
        // returning/formatting a failure; GPU work and resource release never
        // take it. A device's unrelated APIs and other devices remain separate.
        let buffer = {
            let _gate = device.2.lock().unwrap_or_else(|poisoned| poisoned.into_inner());
            // SAFETY: as above.
            unsafe { device.0.new_buffer_with_bytes_no_copy(memory, length, modes, None) }
        };
        Ok(boxed(NmbBuffer(buffer.map_err(e("no-copy buffer"))?)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_release(buffer: *mut NmbBuffer) {
    release(buffer)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_contents(buffer: *const NmbBuffer) -> *mut c_void {
    guard(std::ptr::null_mut(), || {
        Ok(get(buffer)?
            .0
            .contents()
            .map_or(std::ptr::null_mut(), |p| p.as_ptr() as *mut c_void))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_length(buffer: *const NmbBuffer) -> u64 {
    guard(0, || Ok(get(buffer)?.0.length()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_gpu_address(buffer: *const NmbBuffer) -> u64 {
    guard(0, || Ok(get(buffer)?.0.gpu_address().0))
}

// ---- heaps ----

pub struct NmbHeap(Heap);

/// `newHeapWithDescriptor:`: type (0 automatic, 1 placement), size, resource options. Sparse heaps are not made here.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_new(device: *mut NmbDevice, heap_type: u64, size: u64, options_word: u64) -> *mut NmbHeap {
    guard(std::ptr::null_mut(), || {
        let device = &get(device)?.0;
        let heap_type = HeapType::from_raw(heap_type).map_err(e("heap type"))?;
        if heap_type == HeapType::Sparse {
            return Err("sparse heaps are not implemented".into());
        }
        let descriptor = HeapDescriptor::new(heap_type, size, options(options_word)?);
        Ok(boxed(NmbHeap(device.new_heap(&descriptor).map_err(e("heap"))?)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_release(heap: *mut NmbHeap) {
    release(heap)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_size(heap: *const NmbHeap) -> u64 {
    guard(0, || Ok(get(heap)?.0.size()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_used_size(heap: *const NmbHeap) -> u64 {
    guard(0, || Ok(get(heap)?.0.used_size()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_allocated_size(heap: *const NmbHeap) -> u64 {
    guard(0, || Ok(get(heap)?.0.current_allocated_size()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_max_available_size(heap: *const NmbHeap, alignment: u64) -> u64 {
    guard(0, || {
        Ok(get(heap)?
            .0
            .max_available_size(alignment)
            .map_err(e("heap available size"))?)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_set_purgeable_state(heap: *const NmbHeap, state: u64) -> u64 {
    guard(0, || {
        let state = PurgeableState::from_raw(state).map_err(e("purgeable state"))?;
        Ok(get(heap)?
            .0
            .set_purgeable_state(state)
            .map_err(e("heap purgeable state"))?
            .raw())
    })
}

/// A buffer in the heap: automatic heaps place it, a placement heap takes `offset` (`u64::MAX` asks for automatic placement).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_new_buffer(
    heap: *const NmbHeap,
    length: u64,
    options_word: u64,
    offset: u64,
) -> *mut NmbBuffer {
    guard(std::ptr::null_mut(), || {
        let heap = &get(heap)?.0;
        let options = options(options_word)?;
        let buffer = if offset == u64::MAX {
            heap.new_buffer(length, options)
        } else {
            heap.new_buffer_at_offset(length, options, offset)
        };
        Ok(boxed(NmbBuffer(buffer.map_err(e("heap buffer"))?)))
    })
}

/// A texture in the heap, placed as `nmb_heap_new_buffer` places a buffer.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_heap_new_texture(
    heap: *const NmbHeap,
    descriptor: *const NmbTextureDescriptor,
    offset: u64,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let heap = &get(heap)?.0;
        let descriptor = texture_descriptor(descriptor)?;
        let texture = if offset == u64::MAX {
            heap.new_texture(&descriptor)
        } else {
            heap.new_texture_at_offset(&descriptor, offset)
        };
        Ok(boxed(NmbTexture(texture.map_err(e("heap texture"))?)))
    })
}

// ---- textures ----

#[repr(C)]
pub struct NmbTextureDescriptor {
    pub kind: u64,
    pub format: u64,
    pub width: u64,
    pub height: u64,
    pub depth: u64,
    pub levels: u64,
    pub samples: u64,
    pub array_length: u64,
    pub usage: u64,
    pub options: u64,
}

pub struct NmbTexture(Texture);

fn texture_descriptor(d: *const NmbTextureDescriptor) -> Result<TextureDescriptor, String> {
    let d = get(d)?;
    let mut out = TextureDescriptor::default();
    out.texture_type = raw::<TextureType>(d.kind, "texture type")?;
    out.pixel_format = raw::<PixelFormat>(d.format, "pixel format")?;
    out.width = d.width;
    out.height = d.height;
    out.depth = d.depth;
    out.mipmap_level_count = d.levels;
    out.sample_count = d.samples;
    out.array_length = d.array_length;
    out.usage = raw::<TextureUsage>(d.usage, "texture usage")?;
    out.resource_options = options(d.options)?;
    Ok(out)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new(device: *mut NmbDevice, descriptor: *const NmbTextureDescriptor) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        Ok(boxed(NmbTexture(
            get(device)?.0.new_texture(&descriptor).map_err(e("texture"))?,
        )))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_on_buffer(
    buffer: *mut NmbBuffer,
    descriptor: *const NmbTextureDescriptor,
    offset: u64,
    bytes_per_row: u64,
    iosurface: bool,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        let buffer = &get(buffer)?.0;
        let texture = if iosurface {
            buffer.new_texture_with_iosurface_plane(&descriptor, IoSurfacePlane::new(offset, bytes_per_row, false))
        } else {
            buffer.new_texture(&descriptor, offset, bytes_per_row)
        };
        Ok(boxed(NmbTexture(texture.map_err(e("texture over buffer"))?)))
    })
}

/// A texture over plane `plane` of a planar surface whose bytes `buffer` stands for (its first byte the surface's base): the plane
/// at `offset` with rows `bytes_per_row` apart, the surface's OSType `fourcc`, its `width` x `height` (plane 0's), plane 1 at
/// `plane1_offset` with rows `plane1_bytes_per_row` apart (0 and 0 for a one-plane surface), and its `kIOSurfaceYCbCrMatrix`
/// attachment string `matrix` (null without one). The runtime makes Metal's two-plane formats over plane 0 ('b3a8' as
/// RGB10A8_2P_XR10, '_RB4' as RGB8_444_2P, the CoreVideo YCbCr surfaces as YCBCR8_420_2P and kin) and plain formats over a plane,
/// exactly as Metal reads and writes them, and refuses what it cannot.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_on_surface_planes(
    buffer: *mut NmbBuffer,
    descriptor: *const NmbTextureDescriptor,
    offset: u64,
    bytes_per_row: u64,
    fourcc: u32,
    plane: u64,
    width: u64,
    height: u64,
    plane1_offset: u64,
    plane1_bytes_per_row: u64,
    matrix: *const c_char,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        let mut facts = IoSurfaceYCbCr::new(fourcc, plane, [width, height], plane1_offset, plane1_bytes_per_row);
        if !matrix.is_null() {
            // SAFETY: the caller's NUL-terminated string, read for the call.
            let text = unsafe { CStr::from_ptr(matrix) }.to_string_lossy();
            facts = facts.with_matrix(SurfaceYCbCrMatrix::from_attachment(&text));
        }
        let texture = get(buffer)?.0.new_texture_with_iosurface_plane(
            &descriptor,
            IoSurfacePlane::new(offset, bytes_per_row, true).with_ycbcr(facts),
        );
        Ok(boxed(NmbTexture(texture.map_err(e("texture over surface planes"))?)))
    })
}

/// `newLinearTextureWithDescriptor:offset:bytesPerRow:bytesPerImage:` (Apple SPI).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_linear(
    buffer: *mut NmbBuffer,
    descriptor: *const NmbTextureDescriptor,
    offset: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        let texture = get(buffer)?
            .0
            .new_linear_texture(&descriptor, offset, bytes_per_row, bytes_per_image);
        Ok(boxed(NmbTexture(texture.map_err(e("linear texture"))?)))
    })
}

/// The device's optimal image of `descriptor` bound `offset` bytes into `buffer`'s memory, holding the bytes there
/// (`Buffer::new_tiled_texture_in_place`): optimal-tiled texels another driver of the process lays out as the device does.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_tiled_in_place(
    buffer: *mut NmbBuffer,
    descriptor: *const NmbTextureDescriptor,
    offset: u64,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let descriptor = texture_descriptor(descriptor)?;
        let texture = get(buffer)?.0.new_tiled_texture_in_place(&descriptor, offset);
        Ok(boxed(NmbTexture(texture.map_err(e("tiled texture in place"))?)))
    })
}

/// A view of `texture`: of the whole texture as `format` where `whole`, else of `levels` and `slices` (start, count) as `format` and `kind`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_new_view(
    texture: *mut NmbTexture,
    format: u64,
    kind: u64,
    levels: *const u64,
    slices: *const u64,
    whole: bool,
) -> *mut NmbTexture {
    guard(std::ptr::null_mut(), || {
        let t = &get(texture)?.0;
        let format = raw::<PixelFormat>(format, "pixel format")?;
        let view = if whole {
            t.new_texture_view(format)
        } else {
            if levels.is_null() || slices.is_null() {
                return Err("null levels or slices".into());
            }
            // SAFETY: the header's two words each.
            let (l, s) = unsafe {
                (
                    std::slice::from_raw_parts(levels, 2),
                    std::slice::from_raw_parts(slices, 2),
                )
            };
            let ranges = (
                l[0]..l[0].checked_add(l[1]).ok_or("levels overflow")?,
                s[0]..s[0].checked_add(s[1]).ok_or("slices overflow")?,
            );
            t.new_texture_view_with_levels_slices(format, raw::<TextureType>(kind, "texture type")?, ranges.0, ranges.1)
        };
        Ok(boxed(NmbTexture(view.map_err(e("texture view"))?)))
    })
}

/// `setPurgeableState:` on a texture: the runtime's answer as Metal's raw state, 0 where it fails.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_set_purgeable_state(texture: *const NmbTexture, state: u64) -> u64 {
    guard(0, || {
        let state = PurgeableState::from_raw(state).map_err(e("purgeable state"))?;
        Ok(get(texture)?
            .0
            .set_purgeable_state(state)
            .map_err(e("purgeable state"))?
            .raw())
    })
}

/// `setPurgeableState:` on a buffer, as on a texture.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_set_purgeable_state(buffer: *const NmbBuffer, state: u64) -> u64 {
    guard(0, || {
        let state = PurgeableState::from_raw(state).map_err(e("purgeable state"))?;
        Ok(get(buffer)?
            .0
            .set_purgeable_state(state)
            .map_err(e("purgeable state"))?
            .raw())
    })
}

/// `makeAliasable` on a texture: an automatic heap's texture gives its range to the next resource the heap places (Espresso
/// and MPS size their heaps for that); one outside a heap, or of a placement heap, stays as it is. False where it fails.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_make_aliasable(texture: *const NmbTexture) -> bool {
    guard(false, || {
        get(texture)?.0.make_aliasable().map_err(e("make aliasable"))?;
        Ok(true)
    })
}

/// `makeAliasable` on a buffer, as on a texture.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_make_aliasable(buffer: *const NmbBuffer) -> bool {
    guard(false, || {
        get(buffer)?.0.make_aliasable().map_err(e("make aliasable"))?;
        Ok(true)
    })
}

/// `isAliasable`: true once `makeAliasable` gave an automatic heap's resource back, and for a placement heap's from the start.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_is_aliasable(texture: *const NmbTexture) -> bool {
    guard(false, || Ok(get(texture)?.0.is_aliasable()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_buffer_is_aliasable(buffer: *const NmbBuffer) -> bool {
    guard(false, || Ok(get(buffer)?.0.is_aliasable()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_release(texture: *mut NmbTexture) {
    release(texture)
}

/// The texture's `VkImage` (metal2vulkan's interop hook, `Texture::vk_image`), as its handle's bits: 0 where its texels are no
/// one image's. Read-only: the raw GM20B path asks NVK where that image's memory lies, to render and copy it in place.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_vk_image(texture: *const NmbTexture) -> u64 {
    use ash::vk::Handle;
    // SAFETY: the image stays the texture's; the caller only reads where its memory lies, while the texture lives.
    guard(0, || {
        Ok(unsafe { get(texture)?.0.vk_image() }.map_or(0, |image| image.as_raw()))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_texture_describe(texture: *const NmbTexture, out: *mut NmbTextureDescriptor) {
    guard((), || {
        let t = &get(texture)?.0;
        let out = get_mut(out)?;
        *out = NmbTextureDescriptor {
            kind: t.texture_type().as_raw() as u64,
            format: t.pixel_format().as_raw() as u64,
            width: t.width(),
            height: t.height(),
            depth: t.depth(),
            levels: t.mipmap_level_count(),
            samples: t.sample_count(),
            array_length: t.array_length(),
            usage: t.usage().as_raw() as u64,
            options: t.resource_options().raw(),
        };
        Ok(())
    })
}

fn region(r: *const u64) -> Result<Region, String> {
    if r.is_null() {
        return Err("null region".into());
    }
    // SAFETY: the header's six words.
    let r = unsafe { std::slice::from_raw_parts(r, 6) };
    Ok(Region::new_3d(r[0], r[1], r[2], r[3], r[4], r[5]))
}

fn triple(v: *const u64) -> Result<[u64; 3], String> {
    if v.is_null() {
        return Err("null origin or size".into());
    }
    // SAFETY: the header's three words.
    let v = unsafe { std::slice::from_raw_parts(v, 3) };
    Ok([v[0], v[1], v[2]])
}

/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_texture_replace(
    texture: *mut NmbTexture,
    region_words: *const u64,
    level: u64,
    slice: u64,
    bytes: *const c_void,
    bytes_per_row: u64,
    bytes_per_image: u64,
    length: u64,
) -> bool {
    guard(false, || {
        if bytes.is_null() {
            return Err("null bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length as usize) };
        get(texture)?
            .0
            .replace_region(
                region(region_words)?,
                level,
                slice,
                bytes,
                bytes_per_row,
                bytes_per_image,
            )
            .map_err(e("replace region"))?;
        Ok(true)
    })
}

/// # Safety
/// `bytes` holds `length` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_texture_get_bytes(
    texture: *const NmbTexture,
    bytes: *mut c_void,
    bytes_per_row: u64,
    bytes_per_image: u64,
    length: u64,
    region_words: *const u64,
    level: u64,
    slice: u64,
) -> bool {
    guard(false, || {
        if bytes.is_null() {
            return Err("null bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts_mut(bytes as *mut u8, length as usize) };
        get(texture)?
            .0
            .get_bytes(
                bytes,
                bytes_per_row,
                bytes_per_image,
                region(region_words)?,
                level,
                slice,
            )
            .map_err(e("get bytes"))?;
        Ok(true)
    })
}

// ---- libraries and functions ----

pub struct NmbLibrary(LibraryOf, Vec<String>);
/// A function and the specialized name a pipeline links it under, where it was made with one.
pub struct NmbFunction(FunctionOf, Option<String>);

impl NmbFunction {
    pub(crate) fn function(&self) -> Result<&Function, String> {
        self.0.runtime()
    }
}

/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_library_new(
    device: *mut NmbDevice,
    bytes: *const c_void,
    length: usize,
    air: bool,
) -> *mut NmbLibrary {
    guard(std::ptr::null_mut(), || {
        let device = &get(device)?.0;
        if bytes.is_null() || length == 0 {
            return Err("no library bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length) };
        let library = if air {
            device.new_library_with_compile_library(metal2vulkan::library::Library::from_air(bytes).map_err(e("AIR"))?)
        } else {
            device
                .new_library_with_data(crate::air_slice(bytes).ok_or("metallib without an AIR64 slice")?)
                .map_err(e("metallib"))?
        };
        Ok(library_handle(LibraryOf::Runtime(library)))
    })
}

pub(crate) fn library_handle(library: LibraryOf) -> *mut NmbLibrary {
    let names = library.function_names();
    boxed(NmbLibrary(library, names))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_library_release(library: *mut NmbLibrary) {
    release(library)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_library_function_count(library: *const NmbLibrary) -> u32 {
    guard(0, || Ok(get(library)?.1.len() as u32))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_library_function_name(
    library: *const NmbLibrary,
    index: u32,
    out: *mut c_char,
    size: usize,
) -> usize {
    guard(0, || {
        let names = &get(library)?.1;
        Ok(copy_out(names.get(index as usize).ok_or("function index")?, out, size))
    })
}

/// # Safety
/// `name` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_new(library: *mut NmbLibrary, name: *const c_char) -> *mut NmbFunction {
    guard(std::ptr::null_mut(), || {
        if name.is_null() {
            return Err("null function name".into());
        }
        // SAFETY: the caller's contract.
        let name = unsafe { CStr::from_ptr(name) }.to_str().map_err(e("function name"))?;
        Ok(boxed(NmbFunction(get(library)?.0.function(name, "function")?, None)))
    })
}

#[repr(C)]
pub struct NmbConstant {
    pub kind: u64,
    pub index: u64,
    pub name: *const c_char,
    pub bytes: *const c_void,
    pub length: u64,
}

/// # Safety
/// `name` is a C string, `specialized_name` one or null; `constants` holds `count` entries whose names are C strings (or null)
/// and whose bytes hold `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_function_new_specialized(
    library: *mut NmbLibrary,
    name: *const c_char,
    specialized_name: *const c_char,
    constants: *const NmbConstant,
    count: u32,
) -> *mut NmbFunction {
    guard(std::ptr::null_mut(), || {
        // SAFETY: the caller's contract.
        let (name, values, specialized) = unsafe { specialization(name, specialized_name, constants, count) }?;
        let function = get(library)?.0.specialized(name, &values, "specialized function")?;
        Ok(boxed(NmbFunction(function, specialized)))
    })
}

/// nmb_function_new_specialized's arguments: the function's name, its constant values and the name it links under
///
/// # Safety
/// As nmb_function_new_specialized's.
pub(crate) unsafe fn specialization<'a>(
    name: *const c_char,
    specialized_name: *const c_char,
    constants: *const NmbConstant,
    count: u32,
) -> Result<(&'a str, FunctionConstantValues, Option<String>), String> {
    if name.is_null() || (constants.is_null() && count > 0) {
        return Err("null function name or constants".into());
    }
    // SAFETY: the caller's contract.
    let name = unsafe { CStr::from_ptr(name) }.to_str().map_err(e("function name"))?;
    let constants = if count == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(constants, count as usize) }
    };
    let mut values = FunctionConstantValues::defaults();
    for c in constants {
        if c.bytes.is_null() {
            return Err("constant without bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(c.bytes as *const u8, c.length as usize) };
        let value = FunctionConstantValue::new(raw::<DataType>(c.kind, "constant type")?, bytes.to_vec());
        if c.name.is_null() {
            values
                .by_index
                .insert(u32::try_from(c.index).map_err(e("constant index"))?, value);
        } else {
            // SAFETY: the caller's contract.
            let key = unsafe { CStr::from_ptr(c.name) }.to_str().map_err(e("constant name"))?;
            values.by_name.insert(key.to_string(), value);
        }
    }
    let specialized = if specialized_name.is_null() {
        None
    } else {
        // SAFETY: the caller's contract.
        let s = unsafe { CStr::from_ptr(specialized_name) }
            .to_str()
            .map_err(e("specialized name"))?;
        Some(s.to_string())
    };
    Ok((name, values, specialized))
}

/// The function's constants (`functionConstantsDictionary`): how many, and the one at `at` in name order (its name into
/// `out`, its MTLDataType, index and required flag into `info`). A specialized function has none, as Metal answers.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_constant_count(function: *const NmbFunction) -> u32 {
    guard(0, || {
        Ok(get(function)?
            .0
            .function_constants_dictionary("function constants")?
            .len() as u32)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_constant(
    function: *const NmbFunction,
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> usize {
    guard(0, || {
        if info.is_null() {
            return Err("null constant info".into());
        }
        let constants = get(function)?.0.function_constants_dictionary("function constants")?;
        let c = constants
            .values()
            .nth(at as usize)
            .ok_or_else(|| format!("no function constant {at}"))?;
        // SAFETY: the header's three words.
        unsafe { std::slice::from_raw_parts_mut(info, 3) }.copy_from_slice(&[
            u64::from(c.data_type().as_raw()),
            u64::from(c.index()),
            u64::from(c.is_required()),
        ]);
        Ok(copy_out(c.name(), out, size))
    })
}

/// `vertexAttributes` (`stage_input` false) or `stageInputAttributes` (true): the function's
/// attributes, `None` where Metal answers nil.
fn attributes(
    function: *const NmbFunction,
    stage_input: bool,
) -> Result<Option<Vec<metal2vulkan::reflection::Attribute>>, String> {
    let what = if stage_input {
        "stage input attributes"
    } else {
        "vertex attributes"
    };
    get(function)?.0.attributes(stage_input, what)
}

/// How many attributes `vertexAttributes` (`stage_input` false) or `stageInputAttributes` (true)
/// answers, -1 where Metal answers nil.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_attribute_count(function: *const NmbFunction, stage_input: bool) -> i64 {
    guard(-1, || {
        Ok(attributes(function, stage_input)?.map_or(-1, |a| a.len() as i64))
    })
}

/// Attribute `at` of those [`nmb_function_attribute_count`] counts: its name into `out`, and its
/// attributeIndex, MTLDataType, isActive, isPatchData and isPatchControlPointData into `info`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_attribute(
    function: *const NmbFunction,
    stage_input: bool,
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> usize {
    guard(0, || {
        attribute_out(
            &attributes(function, stage_input)?.unwrap_or_default(),
            at,
            out,
            size,
            info,
        )
    })
}

/// Attribute `at` of `attributes` as nmb_function_attribute answers it
pub(crate) fn attribute_out(
    attributes: &[metal2vulkan::reflection::Attribute],
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> Result<usize, String> {
    if info.is_null() {
        return Err("null attribute info".into());
    }
    let a = attributes
        .get(at as usize)
        .ok_or_else(|| format!("no attribute {at}"))?;
    // SAFETY: the header's five words.
    unsafe { std::slice::from_raw_parts_mut(info, 5) }.copy_from_slice(&[
        u64::from(a.attribute_index()),
        u64::from(a.attribute_type().as_raw()),
        u64::from(a.is_active()),
        u64::from(a.is_patch_data()),
        u64::from(a.is_patch_control_point_data()),
    ]);
    Ok(copy_out(a.name(), out, size))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_release(function: *mut NmbFunction) {
    release(function)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_type(function: *const NmbFunction) -> u64 {
    guard(0, || {
        Ok(get(function)?.0.function_type("function type")?.as_raw() as u64)
    })
}

// ---- pipelines and states ----

pub struct NmbComputePipeline(ComputePipelineState);
pub struct NmbRenderPipeline(RenderPipelineState);
pub struct NmbSampler(SamplerState);
pub struct NmbDepthStencil(DepthStencilState);

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_pipeline_new(
    device: *mut NmbDevice,
    function: *mut NmbFunction,
) -> *mut NmbComputePipeline {
    // SAFETY: null options names no array, the existing handles follow this ABI.
    unsafe { nmb_compute_pipeline_new_with_options(device, function, std::ptr::null()) }
}

/// # Safety
/// `options` is null or a valid record with a counted array of live archive handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_new_with_options(
    device: *mut NmbDevice,
    function: *mut NmbFunction,
    options: *const NmbPipelineOptions,
) -> *mut NmbComputePipeline {
    guard(std::ptr::null_mut(), || {
        let resolved = unsafe { pipeline_options(options)? };
        let state = get(device)?
            .0
            .new_compute_pipeline_state_with_function(get(function)?.0.runtime()?, &resolved);
        Ok(boxed(NmbComputePipeline(state.map_err(e("compute pipeline"))?)))
    })
}

/// A pipeline descriptor's function handle as the translator takes it: the runtime's function, or the offline compile's
/// (offline.rs)
pub(crate) trait DescriptorFunction {
    fn stage_function(&self) -> FunctionDescriptor;
    /// The function as a stage links it, under its specialized name
    fn linked_function(&self) -> LinkedFunction;
}

impl DescriptorFunction for NmbFunction {
    fn stage_function(&self) -> FunctionDescriptor {
        self.0.function_descriptor()
    }

    fn linked_function(&self) -> LinkedFunction {
        self.0.linked_function().with_link_name(self.1.clone())
    }
}

#[repr(C)]
pub struct NmbComputePipelineDescriptorOf<F> {
    pub function: *mut F,
    pub max_threads: u64,
    pub multiple_of_execution_width: u64,
    pub indirect: u64,
    pub max_call_stack_depth: u64,
    pub adding_binary_functions: u64,
    pub mutability: [u64; 31],
    pub linked: *const *mut F,
    pub linked_count: u64,
    pub private_linked: *const *mut F,
    pub private_count: u64,
    pub stage_inputs: u64,
    pub stage_attributes: [NmbVertexAttribute; 31],
    pub stage_layouts: [NmbVertexLayout; 31],
    pub stage_index_type: u64,
    pub stage_index_buffer: u64,
}

/// A compute descriptor's stage input (`stageInputDescriptor`): its set attributes, their layouts (a dynamic stride as
/// `None`), its index type and index buffer
fn stage_input<F>(d: &NmbComputePipelineDescriptorOf<F>) -> Result<StageInputDescriptor, String> {
    let mut out = StageInputDescriptor::new();
    for (i, a) in d.stage_attributes.iter().enumerate() {
        if a.format == 0 {
            continue;
        }
        let buffer = u32::try_from(a.buffer).map_err(e("stage input buffer"))?;
        out.attributes.insert(
            i as u32,
            StageInputAttributeDescriptor::new(
                raw::<AttributeFormat>(a.format, "attribute format")?,
                u32::try_from(a.offset).map_err(e("stage input offset"))?,
                buffer,
            ),
        );
        let l = d
            .stage_layouts
            .get(buffer as usize)
            .ok_or("a stage input buffer past 30")?;
        let stride = (l.stride != u64::MAX).then_some(l.stride as u32);
        let mut layout = StageInputLayoutDescriptor::new(stride);
        layout.step_function = raw::<StepFunction>(l.step_function, "step function")?;
        layout.step_rate = l.step_rate as u32;
        out.layouts.insert(buffer, layout);
    }
    out.index_type = raw::<IndexType>(d.stage_index_type, "stage input index type")?;
    out.index_buffer_index = u32::try_from(d.stage_index_buffer).map_err(e("stage input index buffer"))?;
    Ok(out)
}

pub type NmbComputePipelineDescriptor = NmbComputePipelineDescriptorOf<NmbFunction>;

/// The functions of a counted handle array, each as a stage links it
fn linked_functions<F: DescriptorFunction>(
    functions: *const *mut F,
    count: u64,
) -> Result<Vec<LinkedFunction>, String> {
    if count == 0 {
        return Ok(Vec::new());
    }
    if functions.is_null() {
        return Err("null linked functions".into());
    }
    // SAFETY: the header's counted array of live function handles.
    let handles = unsafe { std::slice::from_raw_parts(functions, usize::try_from(count).map_err(e("linked count"))?) };
    handles.iter().map(|&f| Ok(get(f)?.linked_function())).collect()
}

pub(crate) fn compute_descriptor<F: DescriptorFunction>(
    descriptor: *const NmbComputePipelineDescriptorOf<F>,
) -> Result<ComputePipelineDescriptor, String> {
    let d = get(descriptor)?;
    // MTLLinkedFunctions' functions and privateFunctions: the external functions the kernel calls by name (MPS's ND-array kernels
    // import their prologue and epilogue functions, prefixPrimary_f and the like, this way).
    let mut linked = LinkedFunctions::new();
    linked.functions = linked_functions(d.linked, d.linked_count)?;
    linked.private_functions = linked_functions(d.private_linked, d.private_count)?;
    let mut out = ComputePipelineDescriptor::new(StageSource::Function(
        get(d.function)?.stage_function().with_linked(linked),
    ));
    out.max_total_threads_per_threadgroup = if d.max_threads == 0 {
        None
    } else {
        Some(u32::try_from(d.max_threads).map_err(e("compute max threads"))?)
    };
    out.threadgroup_size_is_multiple_of_execution_width = d.multiple_of_execution_width != 0;
    out.support_indirect_command_buffers = d.indirect != 0;
    out.max_call_stack_depth = u32::try_from(d.max_call_stack_depth).map_err(e("compute stack depth"))?;
    out.support_adding_binary_functions = d.adding_binary_functions != 0;
    if d.stage_inputs != 0 {
        out.stage_input = Some(stage_input(d)?);
    }
    for (index, &mutability) in d.mutability.iter().enumerate() {
        if mutability != 0 {
            out.buffers.insert(
                index as u32,
                raw::<metal2vulkan::metal::Mutability>(mutability, "buffer mutability")?,
            );
        }
    }
    Ok(out)
}

/// # Safety
/// `options` is null or a valid record with a counted array of live archive handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_compute_pipeline_new_with_descriptor(
    device: *mut NmbDevice,
    descriptor: *const NmbComputePipelineDescriptor,
    options: *const NmbPipelineOptions,
) -> *mut NmbComputePipeline {
    guard(std::ptr::null_mut(), || {
        let resolved = unsafe { pipeline_options(options)? };
        let state = get(device)?
            .0
            .new_compute_pipeline_state(&compute_descriptor(descriptor)?, &resolved)
            .map_err(e("compute pipeline"))?;
        Ok(boxed(NmbComputePipeline(state)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_pipeline_release(pipeline: *mut NmbComputePipeline) {
    release(pipeline)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_pipeline_max_threads(pipeline: *const NmbComputePipeline) -> u64 {
    guard(0, || Ok(get(pipeline)?.0.max_total_threads_per_threadgroup() as u64))
}

/// `staticThreadgroupMemoryLength`: the bytes of the kernel's own `threadgroup` variables, which the encoder's lengths add to.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_pipeline_static_threadgroup_memory(pipeline: *const NmbComputePipeline) -> u64 {
    guard(0, || Ok(u64::from(get(pipeline)?.0.static_threadgroup_memory_length())))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_pipeline_execution_width(pipeline: *const NmbComputePipeline) -> u64 {
    guard(0, || Ok(get(pipeline)?.0.thread_execution_width() as u64))
}

#[repr(C)]
pub struct NmbColorAttachment {
    pub format: u64,
    pub write_mask: u64,
    pub blending: u64,
    pub source_rgb: u64,
    pub destination_rgb: u64,
    pub rgb_operation: u64,
    pub source_alpha: u64,
    pub destination_alpha: u64,
    pub alpha_operation: u64,
}

#[repr(C)]
pub struct NmbVertexAttribute {
    pub format: u64,
    pub offset: u64,
    pub buffer: u64,
}

#[repr(C)]
pub struct NmbVertexLayout {
    pub stride: u64,
    pub step_function: u64,
    pub step_rate: u64,
}

#[repr(C)]
pub struct NmbRenderPipelineDescriptorOf<F> {
    pub vertex: *mut F,
    pub fragment: *mut F,
    pub colors: [NmbColorAttachment; 8],
    pub depth_format: u64,
    pub stencil_format: u64,
    pub samples: u64,
    pub topology: u64,
    pub rasterization: u64,
    pub vertex_inputs: u64,
    pub attributes: [NmbVertexAttribute; 31],
    pub layouts: [NmbVertexLayout; 31],
    pub vertex_linked: *const *mut F,
    pub vertex_linked_count: u64,
    pub vertex_private_linked: *const *mut F,
    pub vertex_private_count: u64,
    pub fragment_linked: *const *mut F,
    pub fragment_linked_count: u64,
    pub fragment_private_linked: *const *mut F,
    pub fragment_private_count: u64,
    pub tessellation: u64,
    pub max_tessellation_factor: u64,
    pub partition_mode: u64,
    pub factor_format: u64,
    pub factor_step_function: u64,
    pub winding: u64,
    pub control_point_index_type: u64,
    pub factor_scale_enabled: u64,
    pub max_amplification: u64,
}

pub type NmbRenderPipelineDescriptor = NmbRenderPipelineDescriptorOf<NmbFunction>;

fn optional_format(value: u64) -> Result<Option<PixelFormat>, String> {
    if value == 0 {
        Ok(None)
    } else {
        raw::<PixelFormat>(value, "pixel format").map(Some)
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_pipeline_new(
    device: *mut NmbDevice,
    descriptor: *const NmbRenderPipelineDescriptor,
) -> *mut NmbRenderPipeline {
    // SAFETY: null options names no array; the device/descriptor handles follow the existing ABI.
    unsafe { nmb_render_pipeline_new_with_options(device, descriptor, std::ptr::null()) }
}

#[repr(C)]
pub struct NmbPipelineOptions {
    pub flags: u64,
    pub archives: *const *mut NmbBinaryArchive,
    pub archive_count: usize,
}

/// # Safety
/// The options record and its counted archive array are valid for this call.
unsafe fn pipeline_options(options: *const NmbPipelineOptions) -> Result<PipelineOptions, String> {
    let mut resolved = PipelineOptions::default();
    if let Some(options) = unsafe { options.as_ref() } {
        resolved.options = PipelineOption::from_raw(options.flags);
        if options.archive_count > 0 {
            if options.archives.is_null()
                || options.archive_count > isize::MAX as usize / std::mem::size_of::<*mut NmbBinaryArchive>()
            {
                return Err("invalid binary archive array".into());
            }
            let archives = unsafe { std::slice::from_raw_parts(options.archives, options.archive_count) };
            for &archive in archives {
                resolved.binary_archives.push(get(archive)?.0.clone());
            }
        }
    }
    Ok(resolved)
}

/// # Safety
/// `options` is null or a valid record; its array holds `archive_count` live archive handles.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_render_pipeline_new_with_options(
    device: *mut NmbDevice,
    descriptor: *const NmbRenderPipelineDescriptor,
    options: *const NmbPipelineOptions,
) -> *mut NmbRenderPipeline {
    guard(std::ptr::null_mut(), || {
        let device = &get(device)?.0;
        let resolved = unsafe { pipeline_options(options)? };
        let state = device
            .new_render_pipeline_state(&render_descriptor(descriptor)?, &resolved)
            .map_err(e("render pipeline"))?;
        Ok(boxed(NmbRenderPipeline(state)))
    })
}

/// A stage's MTLLinkedFunctions: its functions and privateFunctions.
fn stage_linked<F: DescriptorFunction>(
    functions: *const *mut F,
    count: u64,
    private: *const *mut F,
    private_count: u64,
) -> Result<LinkedFunctions, String> {
    let mut linked = LinkedFunctions::new();
    linked.functions = linked_functions(functions, count)?;
    linked.private_functions = linked_functions(private, private_count)?;
    Ok(linked)
}

pub(crate) fn render_descriptor<F: DescriptorFunction>(
    descriptor: *const NmbRenderPipelineDescriptorOf<F>,
) -> Result<RenderPipelineDescriptor, String> {
    let d = get(descriptor)?;
    let vertex_linked = stage_linked(
        d.vertex_linked,
        d.vertex_linked_count,
        d.vertex_private_linked,
        d.vertex_private_count,
    )?;
    let mut out = RenderPipelineDescriptor::new(StageSource::Function(
        get(d.vertex)?.stage_function().with_linked(vertex_linked),
    ));
    if !d.fragment.is_null() {
        let fragment_linked = stage_linked(
            d.fragment_linked,
            d.fragment_linked_count,
            d.fragment_private_linked,
            d.fragment_private_count,
        )?;
        out.fragment = Some(StageSource::Function(
            get(d.fragment)?.stage_function().with_linked(fragment_linked),
        ));
    }
    for (slot, c) in out.color_attachments.iter_mut().zip(d.colors.iter()) {
        if c.format == 0 {
            continue;
        }
        let mut a = ColorAttachmentDescriptor::new(raw::<PixelFormat>(c.format, "color format")?);
        a.write_mask = raw::<ColorWriteMask>(c.write_mask, "write mask")?;
        a.blending_enabled = c.blending != 0;
        a.source_rgb_blend_factor = raw::<BlendFactor>(c.source_rgb, "blend factor")?;
        a.destination_rgb_blend_factor = raw::<BlendFactor>(c.destination_rgb, "blend factor")?;
        a.rgb_blend_operation = raw::<BlendOperation>(c.rgb_operation, "blend operation")?;
        a.source_alpha_blend_factor = raw::<BlendFactor>(c.source_alpha, "blend factor")?;
        a.destination_alpha_blend_factor = raw::<BlendFactor>(c.destination_alpha, "blend factor")?;
        a.alpha_blend_operation = raw::<BlendOperation>(c.alpha_operation, "blend operation")?;
        *slot = Some(a);
    }
    out.depth_format = optional_format(d.depth_format)?;
    out.stencil_format = optional_format(d.stencil_format)?;
    out.raster_sample_count = d.samples.max(1) as u32;
    out.input_primitive_topology = raw::<PrimitiveTopologyClass>(d.topology, "topology")?;
    out.rasterization_enabled = d.rasterization != 0;
    if d.vertex_inputs != 0 {
        let mut inputs = VertexDescriptor::new();
        for (i, a) in d.attributes.iter().enumerate() {
            if a.format == 0 {
                continue;
            }
            inputs.attributes.insert(
                i as u32,
                VertexAttributeDescriptor::new(
                    raw::<VertexFormat>(a.format, "vertex format")?,
                    a.offset as u32,
                    a.buffer as u32,
                ),
            );
            if let Some(l) = d.layouts.get(a.buffer as usize) {
                // MTLBufferLayoutStrideDynamic (NSUIntegerMax): the stride each bind sets
                let stride = (l.stride != u64::MAX).then_some(l.stride as u32);
                let mut layout = VertexBufferLayoutDescriptor::new(stride);
                layout.step_function = raw::<VertexStepFunction>(l.step_function, "step function")?;
                layout.step_rate = l.step_rate as u32;
                inputs.layouts.insert(a.buffer as u32, layout);
            }
        }
        out.vertex_descriptor = Some(inputs);
    }
    if d.tessellation != 0 {
        let mut t = TessellationDescriptor::default();
        t.max_tessellation_factor = d.max_tessellation_factor as u32;
        t.partition_mode = raw::<TessellationPartitionMode>(d.partition_mode, "partition mode")?;
        t.factor_format = raw::<TessellationFactorFormat>(d.factor_format, "factor format")?;
        t.factor_step_function = raw::<TessellationFactorStepFunction>(d.factor_step_function, "factor step")?;
        t.output_winding_order = raw::<Winding>(d.winding, "winding")?;
        t.control_point_index_type =
            raw::<TessellationControlPointIndexType>(d.control_point_index_type, "control point index type")?;
        t.factor_scale_enabled = d.factor_scale_enabled != 0;
        out.tessellation = Some(t);
    }
    out.max_vertex_amplification_count = d.max_amplification.max(1) as u32;
    Ok(out)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_pipeline_release(pipeline: *mut NmbRenderPipeline) {
    release(pipeline)
}

// ---- binary archives ----

pub struct NmbBinaryArchive(BinaryArchive);

/// # Safety
/// `path` is null (an empty archive) or a C string naming the archive's file.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_binary_archive_new(device: *mut NmbDevice, path: *const c_char) -> *mut NmbBinaryArchive {
    guard(std::ptr::null_mut(), || {
        // SAFETY: the caller's contract.
        let url =
            (!path.is_null()).then(|| PathBuf::from(unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned()));
        let archive = get(device)?
            .0
            .new_binary_archive(&BinaryArchiveDescriptor::new(url))
            .map_err(e("binary archive"))?;
        Ok(boxed(NmbBinaryArchive(archive)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_binary_archive_release(archive: *mut NmbBinaryArchive) {
    release(archive)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_binary_archive_add_render(
    archive: *mut NmbBinaryArchive,
    descriptor: *const NmbRenderPipelineDescriptor,
) -> bool {
    guard(false, || {
        get(archive)?
            .0
            .add_render_pipeline_functions(&render_descriptor(descriptor)?)
            .map_err(e("archive render pipeline"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_binary_archive_add_compute(archive: *mut NmbBinaryArchive, function: *mut NmbFunction) -> bool {
    guard(false, || {
        let descriptor = ComputePipelineDescriptor::new(StageSource::Function(get(function)?.0.function_descriptor()));
        get(archive)?
            .0
            .add_compute_pipeline_functions(&descriptor)
            .map_err(e("archive compute pipeline"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_binary_archive_add_compute_descriptor(
    archive: *mut NmbBinaryArchive,
    descriptor: *const NmbComputePipelineDescriptor,
) -> bool {
    guard(false, || {
        get(archive)?
            .0
            .add_compute_pipeline_functions(&compute_descriptor(descriptor)?)
            .map_err(e("archive compute pipeline"))?;
        Ok(true)
    })
}

/// # Safety
/// `path` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_binary_archive_serialize(archive: *mut NmbBinaryArchive, path: *const c_char) -> bool {
    guard(false, || {
        if path.is_null() {
            return Err("null path".into());
        }
        // SAFETY: the caller's contract.
        let path = PathBuf::from(unsafe { CStr::from_ptr(path) }.to_string_lossy().into_owned());
        get(archive)?
            .0
            .serialize_to_url(&path)
            .map_err(e("archive serialize"))?;
        Ok(true)
    })
}

#[repr(C)]
pub struct NmbSamplerDescriptor {
    pub min_filter: u64,
    pub mag_filter: u64,
    pub mip_filter: u64,
    pub s_address: u64,
    pub t_address: u64,
    pub r_address: u64,
    pub border_color: u64,
    pub normalized: u64,
    pub max_anisotropy: u64,
    pub compare: u64,
    pub lod_average: u64,
    pub lod_min: f32,
    pub lod_max: f32,
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_sampler_new(device: *mut NmbDevice, descriptor: *const NmbSamplerDescriptor) -> *mut NmbSampler {
    guard(std::ptr::null_mut(), || {
        let out = sampler_descriptor(get(descriptor)?)?;
        Ok(boxed(NmbSampler(
            get(device)?.0.new_sampler_state(&out).map_err(e("sampler"))?,
        )))
    })
}

pub(crate) fn sampler_descriptor(d: &NmbSamplerDescriptor) -> Result<SamplerDescriptor, String> {
    let mut out = SamplerDescriptor::new();
    out.min_filter = raw::<SamplerMinMagFilter>(d.min_filter, "min filter")?;
    out.mag_filter = raw::<SamplerMinMagFilter>(d.mag_filter, "mag filter")?;
    out.mip_filter = raw::<SamplerMipFilter>(d.mip_filter, "mip filter")?;
    out.s_address_mode = raw::<SamplerAddressMode>(d.s_address, "address mode")?;
    out.t_address_mode = raw::<SamplerAddressMode>(d.t_address, "address mode")?;
    out.r_address_mode = raw::<SamplerAddressMode>(d.r_address, "address mode")?;
    out.border_color = raw::<SamplerBorderColor>(d.border_color, "border color")?;
    out.normalized_coordinates = d.normalized != 0;
    out.max_anisotropy = d.max_anisotropy.max(1) as u32;
    out.compare_function = raw::<CompareFunction>(d.compare, "compare function")?;
    out.lod_average = d.lod_average != 0;
    out.lod_min_clamp = d.lod_min;
    out.lod_max_clamp = d.lod_max;
    Ok(out)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_sampler_release(sampler: *mut NmbSampler) {
    release(sampler)
}

#[repr(C)]
pub struct NmbStencil {
    pub compare: u64,
    pub failure: u64,
    pub depth_failure: u64,
    pub pass: u64,
    pub read_mask: u64,
    pub write_mask: u64,
}

#[repr(C)]
pub struct NmbDepthStencilDescriptor {
    pub compare: u64,
    pub write: u64,
    pub front: NmbStencil,
    pub back: NmbStencil,
}

fn stencil(s: &NmbStencil) -> Result<StencilDescriptor, String> {
    let mut out = StencilDescriptor::default();
    out.stencil_compare_function = raw::<CompareFunction>(s.compare, "stencil compare")?;
    out.stencil_failure_operation = raw::<StencilOperation>(s.failure, "stencil operation")?;
    out.depth_failure_operation = raw::<StencilOperation>(s.depth_failure, "stencil operation")?;
    out.depth_stencil_pass_operation = raw::<StencilOperation>(s.pass, "stencil operation")?;
    out.read_mask = s.read_mask as u32;
    out.write_mask = s.write_mask as u32;
    Ok(out)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_depth_stencil_new(
    device: *mut NmbDevice,
    descriptor: *const NmbDepthStencilDescriptor,
) -> *mut NmbDepthStencil {
    guard(std::ptr::null_mut(), || {
        let d = get(descriptor)?;
        let mut out = DepthStencilDescriptor::default();
        out.depth_compare_function = raw::<CompareFunction>(d.compare, "depth compare")?;
        out.depth_write_enabled = d.write != 0;
        out.front_face_stencil = stencil(&d.front)?;
        out.back_face_stencil = stencil(&d.back)?;
        Ok(boxed(NmbDepthStencil(
            get(device)?
                .0
                .new_depth_stencil_state(&out)
                .map_err(e("depth/stencil state"))?,
        )))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_depth_stencil_release(state: *mut NmbDepthStencil) {
    release(state)
}

// ---- queues and command buffers ----

pub struct NmbQueue(CommandQueue);
pub struct NmbCommandBuffer(CommandBuffer);

#[unsafe(no_mangle)]
pub extern "C" fn nmb_queue_new(device: *mut NmbDevice) -> *mut NmbQueue {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbQueue(get(device)?.0.new_command_queue().map_err(e("queue"))?)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_queue_release(queue: *mut NmbQueue) {
    release(queue)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_new(queue: *mut NmbQueue) -> *mut NmbCommandBuffer {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbCommandBuffer(
            get(queue)?.0.command_buffer().map_err(e("command buffer"))?,
        )))
    })
}

/// `commandBuffer`, its wait for one of the queue's slots bounded by `timeout_ms`: null with `out[0]` 1 once the queue still
/// holds every slot past it (out[1..4]: the slots held by uncompleted command buffers, of which not committed, the most), null
/// with `out[0]` 0 on another refusal (nmb_last_error), else the command buffer.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_new_within(
    queue: *mut NmbQueue,
    timeout_ms: u64,
    out: *mut u64,
) -> *mut NmbCommandBuffer {
    guard(std::ptr::null_mut(), || {
        let queue = &get(queue)?.0;
        let made = queue
            .command_buffer_within(std::time::Duration::from_millis(timeout_ms))
            .map_err(e("command buffer"))?;
        let (all, open, max) = queue.slots_held();
        if !out.is_null() {
            // SAFETY: the caller's four words.
            let out = unsafe { std::slice::from_raw_parts_mut(out, 4) };
            out.copy_from_slice(&[made.is_none() as u64, all as u64, open as u64, max as u64]);
        }
        match made {
            Some(command_buffer) => Ok(boxed(NmbCommandBuffer(command_buffer))),
            None => Err("no slot of the queue within the bound".into()),
        }
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_release(command_buffer: *mut NmbCommandBuffer) {
    release(command_buffer)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_uses_buffer(
    command_buffer: *const NmbCommandBuffer,
    buffer: *const NmbBuffer,
) -> i32 {
    guard(-1, || {
        let used = get(command_buffer)?.0.used_resources();
        Ok(i32::from(used.references_resource(get(buffer)?.0.as_resource())))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_uses_texture(
    command_buffer: *const NmbCommandBuffer,
    texture: *const NmbTexture,
) -> i32 {
    guard(-1, || {
        let used = get(command_buffer)?.0.used_resources();
        Ok(i32::from(used.references_resource(get(texture)?.0.as_resource())))
    })
}

struct Context(*mut c_void);
// SAFETY: the context is the caller's, handed back on the runtime's thread as the header states.
unsafe impl Send for Context {}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_on_completed(
    command_buffer: *mut NmbCommandBuffer,
    handler: Option<extern "C" fn(*mut c_void)>,
    context: *mut c_void,
) -> bool {
    guard(false, || {
        let handler = handler.ok_or("null handler")?;
        let context = Context(context);
        get(command_buffer)?
            .0
            .add_completed_handler(Box::new(move |_| {
                let context = context;
                handler(context.0)
            }))
            .map_err(e("completed handler"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_on_scheduled(
    command_buffer: *mut NmbCommandBuffer,
    handler: Option<extern "C" fn(*mut c_void)>,
    context: *mut c_void,
) -> bool {
    guard(false, || {
        let handler = handler.ok_or("null handler")?;
        let context = Context(context);
        get(command_buffer)?
            .0
            .add_scheduled_handler(Box::new(move |_| {
                let context = context;
                handler(context.0)
            }))
            .map_err(e("scheduled handler"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_wait_scheduled(command_buffer: *mut NmbCommandBuffer) -> bool {
    guard(false, || {
        get(command_buffer)?
            .0
            .wait_until_scheduled()
            .map_err(e("wait scheduled"))?;
        Ok(true)
    })
}

/// waitUntilScheduled for at most `timeout_ms` (m2v 0115): 0 scheduled, 1 not yet, -1 refused (the runtime's deadlock answers).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_wait_scheduled_for(command_buffer: *mut NmbCommandBuffer, timeout_ms: u64) -> i32 {
    guard(-1, || {
        let scheduled = get(command_buffer)?
            .0
            .wait_until_scheduled_within(Duration::from_millis(timeout_ms))
            .map_err(e("wait scheduled"))?;
        Ok(if scheduled { 0 } else { 1 })
    })
}

/// waitUntilCompleted for at most `timeout_ms` (m2v 0115): 0 completed, 1 not yet, -1 refused (the runtime's deadlock answers).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_wait_completed_for(command_buffer: *mut NmbCommandBuffer, timeout_ms: u64) -> i32 {
    guard(-1, || {
        let completed = get(command_buffer)?
            .0
            .wait_until_completed_within(Duration::from_millis(timeout_ms))
            .map_err(e("wait"))?;
        Ok(if completed { 0 } else { 1 })
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_commit(command_buffer: *mut NmbCommandBuffer) -> bool {
    guard(false, || {
        get(command_buffer)?.0.commit().map_err(e("commit"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_wait(command_buffer: *mut NmbCommandBuffer, timeout_ms: u64) -> i32 {
    guard(-1, || {
        let command_buffer = &get(command_buffer)?.0;
        if timeout_ms == 0 {
            command_buffer.wait_until_completed().map_err(e("wait"))?;
            return Ok(0);
        }
        Ok(
            if command_buffer
                .wait_until_completed_for(Duration::from_millis(timeout_ms))
                .is_ok()
            {
                0
            } else {
                1
            },
        )
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_status(command_buffer: *const NmbCommandBuffer) -> u64 {
    guard(5, || Ok(get(command_buffer)?.0.status().raw()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_error(
    command_buffer: *const NmbCommandBuffer,
    out: *mut c_char,
    size: usize,
) -> usize {
    guard(0, || {
        let command_buffer = &get(command_buffer)?.0;
        let text = (command_buffer.error_description().filter(|text| !text.is_empty()))
            .or_else(|| command_buffer.error().map(|error| format!("{error:?}")));
        Ok(text.map_or(0, |text| copy_out(&text, out, size)))
    })
}

/// Metal's MTLCommandBufferError code of a failed command buffer, 0 before it failed.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_error_code(command_buffer: *const NmbCommandBuffer) -> u64 {
    guard(0, || {
        Ok(get(command_buffer)?.0.error().map_or(0, |error| error.metal_code()))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_gpu_time(command_buffer: *const NmbCommandBuffer, end: bool) -> f64 {
    guard(0.0, || {
        let command_buffer = &get(command_buffer)?.0;
        let time = if end {
            command_buffer.gpu_end_time()
        } else {
            command_buffer.gpu_start_time()
        };
        time.map_err(e("GPU time"))
    })
}

// ---- events and fences ----

pub enum NmbEvent {
    Event(Event),
    Shared(SharedEvent),
}
pub struct NmbFence(Fence);

impl NmbEvent {
    fn as_ref(&self) -> EventRef<'_> {
        match self {
            NmbEvent::Event(e) => EventRef::Event(e),
            NmbEvent::Shared(s) => EventRef::Shared(s),
        }
    }
}

// A plain event's host view too (Event::host_view): the raw GM20B path signals and waits for both kinds through it.
fn shared(event: *const NmbEvent) -> Result<SharedEvent, String> {
    match get(event)? {
        NmbEvent::Shared(s) => Ok(s.clone()),
        NmbEvent::Event(e) => Ok(e.host_view()),
    }
}

/// One listener thread for every notification of this process (Metal's listener only names the queue a block runs on; the frontend
/// forwards to it).
fn listener() -> Result<&'static SharedEventListener, String> {
    static LISTENER: std::sync::OnceLock<Result<SharedEventListener, String>> = std::sync::OnceLock::new();
    LISTENER
        .get_or_init(|| SharedEventListener::new().map_err(e("listener")))
        .as_ref()
        .map_err(|why| why.clone())
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_event_new(device: *mut NmbDevice, is_shared: bool) -> *mut NmbEvent {
    guard(std::ptr::null_mut(), || {
        let device = &get(device)?.0;
        Ok(boxed(if is_shared {
            NmbEvent::Shared(device.new_shared_event().map_err(e("shared event"))?)
        } else {
            NmbEvent::Event(device.new_event().map_err(e("event"))?)
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_event_release(event: *mut NmbEvent) {
    release(event)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_shared_event_value(event: *const NmbEvent) -> u64 {
    guard(0, || Ok(shared(event)?.signaled_value()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_shared_event_set_value(event: *mut NmbEvent, value: u64) -> bool {
    guard(false, || {
        shared(event)?.set_signaled_value(value);
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_shared_event_wait(event: *mut NmbEvent, value: u64, timeout_ms: u64) -> bool {
    guard(false, || {
        Ok(shared(event)?.wait_until_signaled_value(value, Duration::from_millis(timeout_ms)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_shared_event_notify(
    event: *mut NmbEvent,
    value: u64,
    handler: Option<extern "C" fn(*mut c_void, u64)>,
    context: *mut c_void,
) -> bool {
    guard(false, || {
        let handler = handler.ok_or("null handler")?;
        let context = Context(context);
        shared(event)?.notify_listener(
            listener()?,
            value,
            Box::new(move |_, reached| {
                let context = context;
                handler(context.0, reached)
            }),
        );
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_signal_event(
    command_buffer: *mut NmbCommandBuffer,
    event: *mut NmbEvent,
    value: u64,
) -> bool {
    guard(false, || {
        get(command_buffer)?
            .0
            .encode_signal_event(get(event)?.as_ref(), value)
            .map_err(e("signal event"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_command_buffer_wait_event(
    command_buffer: *mut NmbCommandBuffer,
    event: *mut NmbEvent,
    value: u64,
) -> bool {
    guard(false, || {
        get(command_buffer)?
            .0
            .encode_wait_for_event(get(event)?.as_ref(), value)
            .map_err(e("wait for event"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_fence_new(device: *mut NmbDevice) -> *mut NmbFence {
    guard(std::ptr::null_mut(), || Ok(boxed(NmbFence(get(device)?.0.new_fence()))))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_fence_release(fence: *mut NmbFence) {
    release(fence)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_update_fence(encoder: *mut NmbEncoder, fence: *mut NmbFence, stages: u64) -> bool {
    guard(false, || {
        let fence = &get(fence)?.0;
        match get_mut(encoder)? {
            NmbEncoder::Compute(c) => c.update_fence(fence),
            NmbEncoder::Blit(b) => b.update_fence(fence),
            NmbEncoder::Render(r) => r.update_fence(fence, raw::<RenderStages>(stages, "render stages")?),
        }
        .map_err(e("update fence"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_wait_fence(encoder: *mut NmbEncoder, fence: *mut NmbFence, stages: u64) -> bool {
    guard(false, || {
        let fence = &get(fence)?.0;
        match get_mut(encoder)? {
            NmbEncoder::Compute(c) => c.wait_for_fence(fence),
            NmbEncoder::Blit(b) => b.wait_for_fence(fence),
            NmbEncoder::Render(r) => r.wait_for_fence(fence, raw::<RenderStages>(stages, "render stages")?),
        }
        .map_err(e("wait for fence"))?;
        Ok(true)
    })
}

// ---- encoders ----

pub enum NmbEncoder {
    Compute(ComputeCommandEncoder),
    Render(RenderCommandEncoder),
    Blit(BlitCommandEncoder),
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_encoder_new(command_buffer: *mut NmbCommandBuffer) -> *mut NmbEncoder {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbEncoder::Compute(
            get(command_buffer)?
                .0
                .compute_command_encoder()
                .map_err(e("compute encoder"))?,
        )))
    })
}

/// `computeCommandEncoderWithDispatchType:`: a concurrent encoder leaves the order of its dispatches to the program's barriers.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_encoder_new_with_dispatch_type(
    command_buffer: *mut NmbCommandBuffer,
    dispatch_type: u64,
) -> *mut NmbEncoder {
    guard(std::ptr::null_mut(), || {
        let kind = raw::<DispatchType>(dispatch_type, "dispatch type")?;
        let encoder = get(command_buffer)?
            .0
            .compute_command_encoder_with_dispatch_type(kind)
            .map_err(e("compute encoder"))?;
        Ok(boxed(NmbEncoder::Compute(encoder)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_encoder_new(command_buffer: *mut NmbCommandBuffer) -> *mut NmbEncoder {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbEncoder::Blit(
            get(command_buffer)?
                .0
                .blit_command_encoder()
                .map_err(e("blit encoder"))?,
        )))
    })
}

#[repr(C)]
pub struct NmbPassAttachment {
    pub texture: *mut NmbTexture,
    pub level: u64,
    pub slice: u64,
    pub depth_plane: u64,
    pub load: u64,
    pub store: u64,
    pub clear: [f64; 4],
    /// `resolveTexture` (null: none), `resolveLevel`, `resolveSlice`, `resolveDepthPlane`.
    pub resolve_texture: *mut NmbTexture,
    pub resolve_level: u64,
    pub resolve_slice: u64,
    pub resolve_depth_plane: u64,
    /// The depth attachment's `depthResolveFilter`, the stencil attachment's `stencilResolveFilter`; 0 for a color one.
    pub resolve_filter: u64,
}

#[repr(C)]
pub struct NmbRenderPassDescriptor {
    pub colors: [NmbPassAttachment; 8],
    pub depth: NmbPassAttachment,
    pub stencil: NmbPassAttachment,
    pub width: u64,
    pub height: u64,
    pub array_length: u64,
    pub samples: u64,
    pub visibility: *const NmbBuffer,
}

fn texture_of(a: &NmbPassAttachment) -> Result<Option<Texture>, String> {
    if a.texture.is_null() {
        Ok(None)
    } else {
        Ok(Some(get(a.texture)?.0.clone()))
    }
}

fn resolve_texture_of(a: &NmbPassAttachment) -> Result<Option<Texture>, String> {
    if a.resolve_texture.is_null() {
        Ok(None)
    } else {
        Ok(Some(get(a.resolve_texture)?.0.clone()))
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_encoder_new(
    command_buffer: *mut NmbCommandBuffer,
    descriptor: *const NmbRenderPassDescriptor,
) -> *mut NmbEncoder {
    guard(std::ptr::null_mut(), || {
        let d = get(descriptor)?;
        let mut pass = RenderPassDescriptor::new();
        for (slot, a) in pass.color_attachments.iter_mut().zip(d.colors.iter()) {
            slot.texture = texture_of(a)?;
            slot.level = a.level;
            slot.slice = a.slice;
            slot.depth_plane = a.depth_plane;
            slot.resolve_texture = resolve_texture_of(a)?;
            slot.resolve_level = a.resolve_level;
            slot.resolve_slice = a.resolve_slice;
            slot.resolve_depth_plane = a.resolve_depth_plane;
            slot.load_action = LoadAction::from_raw(a.load).map_err(e("load action"))?;
            slot.store_action = StoreAction::from_raw(a.store).map_err(e("store action"))?;
            slot.clear_color = ClearColor::new(a.clear[0], a.clear[1], a.clear[2], a.clear[3]);
        }
        let depth = &mut pass.depth_attachment;
        depth.texture = texture_of(&d.depth)?;
        depth.level = d.depth.level;
        depth.slice = d.depth.slice;
        depth.depth_plane = d.depth.depth_plane;
        depth.resolve_texture = resolve_texture_of(&d.depth)?;
        depth.resolve_level = d.depth.resolve_level;
        depth.resolve_slice = d.depth.resolve_slice;
        depth.resolve_depth_plane = d.depth.resolve_depth_plane;
        depth.depth_resolve_filter =
            MultisampleDepthResolveFilter::from_raw(d.depth.resolve_filter).map_err(e("depth resolve filter"))?;
        depth.load_action = LoadAction::from_raw(d.depth.load).map_err(e("load action"))?;
        depth.store_action = StoreAction::from_raw(d.depth.store).map_err(e("store action"))?;
        depth.clear_depth = d.depth.clear[0];
        let stencil = &mut pass.stencil_attachment;
        stencil.texture = texture_of(&d.stencil)?;
        stencil.level = d.stencil.level;
        stencil.slice = d.stencil.slice;
        stencil.depth_plane = d.stencil.depth_plane;
        stencil.resolve_texture = resolve_texture_of(&d.stencil)?;
        stencil.resolve_level = d.stencil.resolve_level;
        stencil.resolve_slice = d.stencil.resolve_slice;
        stencil.resolve_depth_plane = d.stencil.resolve_depth_plane;
        stencil.stencil_resolve_filter =
            MultisampleStencilResolveFilter::from_raw(d.stencil.resolve_filter).map_err(e("stencil resolve filter"))?;
        stencil.load_action = LoadAction::from_raw(d.stencil.load).map_err(e("load action"))?;
        stencil.store_action = StoreAction::from_raw(d.stencil.store).map_err(e("store action"))?;
        stencil.clear_stencil = d.stencil.clear[0] as u32;
        pass.render_target_width = d.width;
        pass.render_target_height = d.height;
        pass.render_target_array_length = d.array_length;
        pass.default_raster_sample_count = d.samples;
        if !d.visibility.is_null() {
            pass.visibility_result_buffer = Some(get(d.visibility)?.0.clone());
        }
        let encoder = get(command_buffer)?
            .0
            .render_command_encoder(&pass)
            .map_err(e("render encoder"))?;
        Ok(boxed(NmbEncoder::Render(encoder)))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_end(encoder: *mut NmbEncoder) -> bool {
    guard(false, || {
        if encoder.is_null() {
            return Err("null encoder".into());
        }
        // SAFETY: from `boxed`; ending releases it.
        let encoder = unsafe { Box::from_raw(encoder) };
        match *encoder {
            NmbEncoder::Compute(c) => c.end_encoding(),
            NmbEncoder::Render(r) => r.end_encoding(),
            NmbEncoder::Blit(b) => b.end_encoding(),
        }
        .map_err(e("end encoding"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_discard(encoder: *mut NmbEncoder) {
    release(encoder)
}

fn compute<'a>(encoder: *mut NmbEncoder) -> Result<&'a mut ComputeCommandEncoder, String> {
    match get_mut(encoder)? {
        NmbEncoder::Compute(c) => Ok(c),
        _ => Err("not a compute encoder".into()),
    }
}

fn render<'a>(encoder: *mut NmbEncoder) -> Result<&'a mut RenderCommandEncoder, String> {
    match get_mut(encoder)? {
        NmbEncoder::Render(r) => Ok(r),
        _ => Err("not a render encoder".into()),
    }
}

fn blit<'a>(encoder: *mut NmbEncoder) -> Result<&'a mut BlitCommandEncoder, String> {
    match get_mut(encoder)? {
        NmbEncoder::Blit(b) => Ok(b),
        _ => Err("not a blit encoder".into()),
    }
}

fn optional<'a, T>(handle: *const T) -> Result<Option<&'a T>, String> {
    if handle.is_null() {
        Ok(None)
    } else {
        get(handle).map(Some)
    }
}

const COMPUTE: u32 = 0;
const VERTEX: u32 = 1;
const FRAGMENT: u32 = 2;

fn bad_stage(stage: u32) -> String {
    format!("stage {stage} does not fit this encoder")
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_set_buffer(
    encoder: *mut NmbEncoder,
    stage: u32,
    buffer: *mut NmbBuffer,
    offset: u64,
    index: u32,
) -> bool {
    guard(false, || {
        let buffer = optional(buffer)?.map(|b| &b.0);
        match (get_mut(encoder)?, stage) {
            (NmbEncoder::Compute(c), COMPUTE) => c.set_buffer(buffer, offset, index),
            (NmbEncoder::Render(r), VERTEX) => r.set_vertex_buffer(buffer, offset, index),
            (NmbEncoder::Render(r), FRAGMENT) => r.set_fragment_buffer(buffer, offset, index),
            _ => return Err(bad_stage(stage)),
        }
        .map_err(e("set buffer"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_set_buffer_offset(encoder: *mut NmbEncoder, stage: u32, offset: u64, index: u32) -> bool {
    guard(false, || {
        match (get_mut(encoder)?, stage) {
            (NmbEncoder::Compute(c), COMPUTE) => c.set_buffer_offset(offset, index),
            (NmbEncoder::Render(r), VERTEX) => r.set_vertex_buffer_offset(offset, index),
            (NmbEncoder::Render(r), FRAGMENT) => r.set_fragment_buffer_offset(offset, index),
            _ => return Err(bad_stage(stage)),
        }
        .map_err(e("set buffer offset"))?;
        Ok(true)
    })
}

/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_encoder_set_bytes(
    encoder: *mut NmbEncoder,
    stage: u32,
    bytes: *const c_void,
    length: usize,
    index: u32,
) -> bool {
    guard(false, || {
        if bytes.is_null() {
            return Err("null bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes as *const u8, length) };
        match (get_mut(encoder)?, stage) {
            (NmbEncoder::Compute(c), COMPUTE) => c.set_bytes(bytes, index),
            (NmbEncoder::Render(r), VERTEX) => r.set_vertex_bytes(bytes, index),
            (NmbEncoder::Render(r), FRAGMENT) => r.set_fragment_bytes(bytes, index),
            _ => return Err(bad_stage(stage)),
        }
        .map_err(e("set bytes"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_set_texture(
    encoder: *mut NmbEncoder,
    stage: u32,
    texture: *mut NmbTexture,
    index: u32,
) -> bool {
    guard(false, || {
        let texture = optional(texture)?.map(|t| &t.0);
        match (get_mut(encoder)?, stage) {
            (NmbEncoder::Compute(c), COMPUTE) => c.set_texture(texture, index),
            (NmbEncoder::Render(r), VERTEX) => r.set_vertex_texture(texture, index),
            (NmbEncoder::Render(r), FRAGMENT) => r.set_fragment_texture(texture, index),
            _ => return Err(bad_stage(stage)),
        }
        .map_err(e("set texture"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_set_sampler(
    encoder: *mut NmbEncoder,
    stage: u32,
    sampler: *mut NmbSampler,
    index: u32,
) -> bool {
    guard(false, || {
        let sampler = optional(sampler)?.map(|s| &s.0);
        match (get_mut(encoder)?, stage) {
            (NmbEncoder::Compute(c), COMPUTE) => c.set_sampler_state(sampler, index),
            (NmbEncoder::Render(r), VERTEX) => r.set_vertex_sampler_state(sampler, index),
            (NmbEncoder::Render(r), FRAGMENT) => r.set_fragment_sampler_state(sampler, index),
            _ => return Err(bad_stage(stage)),
        }
        .map_err(e("set sampler"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_set_pipeline(encoder: *mut NmbEncoder, pipeline: *mut NmbComputePipeline) -> bool {
    guard(false, || {
        compute(encoder)?
            .set_compute_pipeline_state(&get(pipeline)?.0)
            .map_err(e("compute pipeline"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_set_threadgroup_memory(encoder: *mut NmbEncoder, length: u32, index: u32) -> bool {
    guard(false, || {
        compute(encoder)?
            .set_threadgroup_memory_length(length, index)
            .map_err(e("threadgroup memory"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_dispatch(
    encoder: *mut NmbEncoder,
    size: *const u64,
    per_group: *const u64,
    threads: bool,
) -> bool {
    guard(false, || {
        let (s, g) = (triple(size)?, triple(per_group)?);
        let (s, g) = (Size::new(s[0], s[1], s[2]), Size::new(g[0], g[1], g[2]));
        let c = compute(encoder)?;
        if threads {
            c.dispatch_threads(s, g)
        } else {
            c.dispatch_threadgroups(s, g)
        }
        .map_err(e("dispatch"))?;
        Ok(true)
    })
}

/// `dispatchThreadgroupsWithIndirectBuffer:indirectBufferOffset:threadsPerThreadgroup:` with `per_group`; with none, the
/// encoder's SPI `dispatchThreadsWithIndirectBuffer:indirectBufferOffset:` (its arguments hold the threadgroup size too).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_dispatch_indirect(
    encoder: *mut NmbEncoder,
    arguments: *mut NmbBuffer,
    offset: u64,
    per_group: *const u64,
) -> bool {
    guard(false, || {
        let buffer = &get(arguments)?.0;
        let c = compute(encoder)?;
        if per_group.is_null() {
            c.dispatch_threads_indirect(buffer, offset)
        } else {
            let g = triple(per_group)?;
            c.dispatch_threadgroups_indirect(buffer, offset, Size::new(g[0], g[1], g[2]))
        }
        .map_err(e("indirect dispatch"))?;
        Ok(true)
    })
}

/// Counted handle arrays as resources, buffers then textures (a null array of no handles is none).
fn resource_refs<'a>(
    buffers: *const *mut NmbBuffer,
    buffer_count: usize,
    textures: *const *mut NmbTexture,
    texture_count: usize,
) -> Result<Vec<ResourceRef<'a>>, String> {
    if (buffers.is_null() && buffer_count > 0) || (textures.is_null() && texture_count > 0) {
        return Err("null resources".into());
    }
    let mut out = Vec::with_capacity(buffer_count + texture_count);
    if buffer_count > 0 {
        // SAFETY: the header's counted array of live handles.
        for &b in unsafe { std::slice::from_raw_parts(buffers, buffer_count) } {
            out.push(ResourceRef::Buffer(&get(b)?.0));
        }
    }
    if texture_count > 0 {
        // SAFETY: as above.
        for &t in unsafe { std::slice::from_raw_parts(textures, texture_count) } {
            out.push(ResourceRef::Texture(&get(t)?.0));
        }
    }
    Ok(out)
}

/// `useResource(s):usage:` (compute) and `useResource(s):usage:stages:` (render): the encoder's later commands may reach
/// these resources through addresses or resource IDs held in memory (argument buffers), as `usage` says.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_use_resources(
    encoder: *mut NmbEncoder,
    buffers: *const *mut NmbBuffer,
    buffer_count: usize,
    textures: *const *mut NmbTexture,
    texture_count: usize,
    usage: u64,
    stages: u64,
) -> bool {
    guard(false, || {
        let resources = resource_refs(buffers, buffer_count, textures, texture_count)?;
        let usage = raw::<ResourceUsage>(usage, "resource usage")?;
        match get_mut(encoder)? {
            NmbEncoder::Compute(c) => c.use_resources(&resources, usage),
            NmbEncoder::Render(r) => r.use_resources(&resources, usage, raw::<RenderStages>(stages, "render stages")?),
            NmbEncoder::Blit(_) => return Err("a blit encoder uses no resources".into()),
        }
        .map_err(e("use resources"))?;
        Ok(true)
    })
}

/// `useHeap(s):` (compute) and `useHeap(s):stages:` (render): every resource of the heaps, for reading.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_encoder_use_heaps(
    encoder: *mut NmbEncoder,
    heaps: *const *mut NmbHeap,
    count: usize,
    stages: u64,
) -> bool {
    guard(false, || {
        if heaps.is_null() && count > 0 {
            return Err("null heaps".into());
        }
        let handles = if count == 0 {
            &[][..]
        } else {
            unsafe { std::slice::from_raw_parts(heaps, count) }
        };
        let heaps = handles
            .iter()
            .map(|&h| Ok(&get(h)?.0))
            .collect::<Result<Vec<&Heap>, String>>()?;
        match get_mut(encoder)? {
            NmbEncoder::Compute(c) => c.use_heaps(&heaps),
            NmbEncoder::Render(r) => r.use_heaps(&heaps, raw::<RenderStages>(stages, "render stages")?),
            NmbEncoder::Blit(_) => return Err("a blit encoder uses no heaps".into()),
        }
        .map_err(e("use heaps"))?;
        Ok(true)
    })
}

/// `memoryBarrierWithScope:` (Metal's MTLBarrierScope bits).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_memory_barrier(encoder: *mut NmbEncoder, scope: u64) -> bool {
    guard(false, || {
        let scope = raw::<BarrierScope>(scope, "barrier scope")?;
        compute(encoder)?.memory_barrier_with_scope(scope);
        Ok(true)
    })
}

/// `memoryBarrierWithResources:count:`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_memory_barrier_resources(
    encoder: *mut NmbEncoder,
    buffers: *const *mut NmbBuffer,
    buffer_count: usize,
    textures: *const *mut NmbTexture,
    texture_count: usize,
) -> bool {
    guard(false, || {
        let resources = resource_refs(buffers, buffer_count, textures, texture_count)?;
        compute(encoder)?
            .memory_barrier_with_resources(&resources)
            .map_err(e("memory barrier"))?;
        Ok(true)
    })
}

/// `setStageInRegion:` (origin then size), or with a buffer `setStageInRegionWithIndirectBuffer:indirectBufferOffset:`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_set_stage_in_region(
    encoder: *mut NmbEncoder,
    region_words: *const u64,
    indirect: *mut NmbBuffer,
    offset: u64,
) -> bool {
    guard(false, || {
        let c = compute(encoder)?;
        if indirect.is_null() {
            c.set_stage_in_region(region(region_words)?)
        } else {
            c.set_stage_in_region_indirect(&get(indirect)?.0, offset)
        }
        .map_err(e("stage-in region"))?;
        Ok(true)
    })
}

/// `setSamplerState:lodMinClamp:lodMaxClamp:atIndex:`: the state with these LOD clamps in place of its own.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_set_sampler_lod(
    encoder: *mut NmbEncoder,
    sampler: *mut NmbSampler,
    lod_min: f32,
    lod_max: f32,
    index: u32,
) -> bool {
    guard(false, || {
        let sampler = optional(sampler)?.map(|s| &s.0);
        compute(encoder)?
            .set_sampler_state_with_lod_clamps(sampler, lod_min, lod_max, index)
            .map_err(e("sampler lod clamps"))?;
        Ok(true)
    })
}

/// `setImageblockWidth:height:`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_compute_set_imageblock_size(encoder: *mut NmbEncoder, width: u64, height: u64) -> bool {
    guard(false, || {
        compute(encoder)?
            .set_imageblock_size(width, height)
            .map_err(e("imageblock size"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_pipeline(encoder: *mut NmbEncoder, pipeline: *mut NmbRenderPipeline) -> bool {
    guard(false, || {
        render(encoder)?
            .set_render_pipeline_state(&get(pipeline)?.0)
            .map_err(e("render pipeline"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_depth_stencil(encoder: *mut NmbEncoder, state: *mut NmbDepthStencil) -> bool {
    guard(false, || {
        render(encoder)?
            .set_depth_stencil_state(optional(state)?.map(|s| &s.0))
            .map_err(e("depth/stencil state"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_viewport(encoder: *mut NmbEncoder, viewport: *const f64) -> bool {
    guard(false, || {
        if viewport.is_null() {
            return Err("null viewport".into());
        }
        // SAFETY: the header's six values.
        let v = unsafe { std::slice::from_raw_parts(viewport, 6) };
        render(encoder)?
            .set_viewport(Viewport::new(v[0], v[1], v[2], v[3], v[4], v[5]))
            .map_err(e("viewport"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_scissor(encoder: *mut NmbEncoder, rect: *const u64) -> bool {
    guard(false, || {
        if rect.is_null() {
            return Err("null scissor".into());
        }
        // SAFETY: the header's four values.
        let r = unsafe { std::slice::from_raw_parts(rect, 4) };
        render(encoder)?
            .set_scissor_rect(ScissorRect::new(r[0], r[1], r[2], r[3]))
            .map_err(e("scissor"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_raster(
    encoder: *mut NmbEncoder,
    cull_mode: u64,
    winding: u64,
    fill_mode: u64,
) -> bool {
    guard(false, || {
        let r = render(encoder)?;
        r.set_cull_mode(raw::<CullMode>(cull_mode, "cull mode")?);
        r.set_front_facing_winding(raw::<Winding>(winding, "winding")?);
        r.set_triangle_fill_mode(raw::<TriangleFillMode>(fill_mode, "fill mode")?)
            .map_err(e("fill mode"))?;
        Ok(true)
    })
}

/// `setDepthClipMode:`: `Clamp` needs the device's `depthClamp` (refused without it, the mode set before kept).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_depth_clip_mode(encoder: *mut NmbEncoder, mode: u64) -> bool {
    guard(false, || {
        render(encoder)?
            .set_depth_clip_mode(raw::<DepthClipMode>(mode, "depth clip mode")?)
            .map_err(e("depth clip mode"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_stencil_reference(encoder: *mut NmbEncoder, front: u32, back: u32) -> bool {
    guard(false, || {
        render(encoder)?.set_stencil_reference_values(front, back);
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_depth_bias(encoder: *mut NmbEncoder, bias: f32, slope_scale: f32, clamp: f32) -> bool {
    guard(false, || {
        render(encoder)?
            .set_depth_bias(bias, slope_scale, clamp)
            .map_err(e("depth bias"))?;
        Ok(true)
    })
}

/// SPI `setLineWidth:`: the runtime accepts Metal's default width of 1 and refuses any other (Apple's wide-line rule is unmeasured).
#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_line_width(encoder: *mut NmbEncoder, width: f32) -> bool {
    guard(false, || {
        render(encoder)?.set_line_width(width).map_err(e("line width"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_visibility_mode(encoder: *mut NmbEncoder, mode: u64, offset: u64) -> bool {
    guard(false, || {
        let mode = VisibilityResultMode::from_raw(mode).map_err(e("visibility result mode"))?;
        render(encoder)?
            .set_visibility_result_mode(mode, offset)
            .map_err(e("visibility result mode"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_set_blend_color(encoder: *mut NmbEncoder, color: *const f32) -> bool {
    guard(false, || {
        if color.is_null() {
            return Err("null color".into());
        }
        // SAFETY: the header's four values.
        let c = unsafe { std::slice::from_raw_parts(color, 4) };
        render(encoder)?.set_blend_color(c[0], c[1], c[2], c[3]);
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_render_draw(
    encoder: *mut NmbEncoder,
    primitive: u64,
    start: u32,
    count: u32,
    instances: u32,
    base_instance: u32,
) -> bool {
    guard(false, || {
        render(encoder)?
            .draw_primitives(
                raw::<PrimitiveType>(primitive, "primitive")?,
                start,
                count,
                instances,
                base_instance,
            )
            .map_err(e("draw"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn nmb_render_draw_indexed(
    encoder: *mut NmbEncoder,
    primitive: u64,
    count: u32,
    index_type: u64,
    indices: *mut NmbBuffer,
    offset: u64,
    instances: u32,
    base_vertex: i32,
    base_instance: u32,
) -> bool {
    guard(false, || {
        render(encoder)?
            .draw_indexed_primitives(
                raw::<PrimitiveType>(primitive, "primitive")?,
                count,
                raw::<IndexType>(index_type, "index type")?,
                &get(indices)?.0,
                offset,
                instances,
                base_vertex,
                base_instance,
            )
            .map_err(e("indexed draw"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_copy_buffer(
    encoder: *mut NmbEncoder,
    source: *mut NmbBuffer,
    source_offset: u64,
    destination: *mut NmbBuffer,
    destination_offset: u64,
    size: u64,
) -> bool {
    guard(false, || {
        blit(encoder)?
            .copy_from_buffer_to_buffer(
                &get(source)?.0,
                source_offset,
                &get(destination)?.0,
                destination_offset,
                size,
            )
            .map_err(e("buffer copy"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_fill_buffer(
    encoder: *mut NmbEncoder,
    buffer: *mut NmbBuffer,
    start: u64,
    end: u64,
    value: u8,
) -> bool {
    guard(false, || {
        blit(encoder)?
            .fill_buffer(&get(buffer)?.0, start..end, value)
            .map_err(e("fill"))?;
        Ok(true)
    })
}

fn origin(v: *const u64) -> Result<Origin, String> {
    let v = triple(v)?;
    Ok(Origin::new(v[0], v[1], v[2]))
}

fn size(v: *const u64) -> Result<Size, String> {
    let v = triple(v)?;
    Ok(Size::new(v[0], v[1], v[2]))
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn nmb_blit_copy_texture(
    encoder: *mut NmbEncoder,
    source: *mut NmbTexture,
    source_slice: u64,
    source_level: u64,
    source_origin: *const u64,
    extent: *const u64,
    destination: *mut NmbTexture,
    destination_slice: u64,
    destination_level: u64,
    destination_origin: *const u64,
) -> bool {
    guard(false, || {
        blit(encoder)?
            .copy_from_texture_to_texture(
                &get(source)?.0,
                source_slice,
                source_level,
                origin(source_origin)?,
                size(extent)?,
                &get(destination)?.0,
                destination_slice,
                destination_level,
                origin(destination_origin)?,
            )
            .map_err(e("texture copy"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn nmb_blit_buffer_to_texture(
    encoder: *mut NmbEncoder,
    source: *mut NmbBuffer,
    offset: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
    extent: *const u64,
    destination: *mut NmbTexture,
    slice: u64,
    level: u64,
    destination_origin: *const u64,
) -> bool {
    guard(false, || {
        blit(encoder)?
            .copy_from_buffer_to_texture(
                &get(source)?.0,
                offset,
                bytes_per_row,
                bytes_per_image,
                size(extent)?,
                &get(destination)?.0,
                slice,
                level,
                origin(destination_origin)?,
                BlitOption::None,
            )
            .map_err(e("buffer to texture"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
pub extern "C" fn nmb_blit_texture_to_buffer(
    encoder: *mut NmbEncoder,
    source: *mut NmbTexture,
    slice: u64,
    level: u64,
    source_origin: *const u64,
    extent: *const u64,
    destination: *mut NmbBuffer,
    offset: u64,
    bytes_per_row: u64,
    bytes_per_image: u64,
) -> bool {
    guard(false, || {
        blit(encoder)?
            .copy_from_texture_to_buffer(
                &get(source)?.0,
                slice,
                level,
                origin(source_origin)?,
                size(extent)?,
                &get(destination)?.0,
                offset,
                bytes_per_row,
                bytes_per_image,
                BlitOption::None,
            )
            .map_err(e("texture to buffer"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_blit_generate_mipmaps(encoder: *mut NmbEncoder, texture: *mut NmbTexture) -> bool {
    guard(false, || {
        blit(encoder)?
            .generate_mipmaps(&get(texture)?.0)
            .map_err(e("mipmaps"))?;
        Ok(true)
    })
}

// ---- argument encoders ----

pub struct NmbArgumentEncoder(ArgumentEncoder);

/// `newArgumentEncoderWithBufferIndex:`: the encoder of the argument buffer the function takes at buffer `index`, as its AIR
/// lays it out.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_function_new_argument_encoder(function: *mut NmbFunction, index: u32) -> *mut NmbArgumentEncoder {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbArgumentEncoder(
            get(function)?
                .0
                .runtime()?
                .new_argument_encoder_with_buffer_index(index)
                .map_err(e("argument encoder"))?,
        )))
    })
}

#[repr(C)]
pub struct NmbArgumentDescriptor {
    pub data_type: u64,
    pub index: u64,
    pub array_length: u64,
    pub access: u64,
    pub texture_type: u64,
    pub constant_block_alignment: u64,
}

/// `newArgumentEncoderWithArguments:`: the encoder of the layout Metal gives these argument descriptors.
///
/// # Safety
/// `descriptors` holds `count` records.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_device_new_argument_encoder(
    device: *mut NmbDevice,
    descriptors: *const NmbArgumentDescriptor,
    count: usize,
) -> *mut NmbArgumentEncoder {
    guard(std::ptr::null_mut(), || {
        if descriptors.is_null() || count == 0 {
            return Err("no argument descriptors".into());
        }
        // SAFETY: the caller's contract.
        let records = unsafe { std::slice::from_raw_parts(descriptors, count) };
        let mut arguments = Vec::with_capacity(count);
        for r in records {
            let mut a = ArgumentDescriptor::new(
                raw::<DataType>(r.data_type, "argument data type")?,
                u32::try_from(r.index).map_err(e("argument index"))?,
            );
            a.array_length = u32::try_from(r.array_length).map_err(e("argument array length"))?;
            a.access = raw::<BindingAccess>(r.access, "argument access")?;
            a.texture_type = raw::<TextureType>(r.texture_type, "argument texture type")?;
            a.constant_block_alignment =
                u32::try_from(r.constant_block_alignment).map_err(e("constant block alignment"))?;
            arguments.push(a);
        }
        Ok(boxed(NmbArgumentEncoder(
            get(device)?
                .0
                .new_argument_encoder_with_arguments(&arguments)
                .map_err(e("argument encoder"))?,
        )))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_release(encoder: *mut NmbArgumentEncoder) {
    release(encoder)
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_length(encoder: *const NmbArgumentEncoder) -> u64 {
    guard(0, || Ok(get(encoder)?.0.encoded_length()))
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_alignment(encoder: *const NmbArgumentEncoder) -> u64 {
    guard(0, || Ok(get(encoder)?.0.alignment()))
}

/// `setArgumentBuffer:startOffset:arrayElement:` (`setArgumentBuffer:offset:` is element 0); a null buffer sets none.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_set_argument_buffer(
    encoder: *mut NmbArgumentEncoder,
    buffer: *mut NmbBuffer,
    offset: u64,
    element: u64,
) -> bool {
    guard(false, || {
        let buffer = optional(buffer)?.map(|b| &b.0);
        get_mut(encoder)?
            .0
            .set_argument_buffer_start_offset_array_element(buffer, offset, element)
            .map_err(e("argument buffer"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_set_buffer(
    encoder: *mut NmbArgumentEncoder,
    buffer: *mut NmbBuffer,
    offset: u64,
    index: u32,
) -> bool {
    guard(false, || {
        get(encoder)?
            .0
            .set_buffer(optional(buffer)?.map(|b| &b.0), offset, index)
            .map_err(e("argument buffer"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_set_texture(
    encoder: *mut NmbArgumentEncoder,
    texture: *mut NmbTexture,
    index: u32,
) -> bool {
    guard(false, || {
        get(encoder)?
            .0
            .set_texture(optional(texture)?.map(|t| &t.0), index)
            .map_err(e("argument texture"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_set_sampler(
    encoder: *mut NmbArgumentEncoder,
    sampler: *mut NmbSampler,
    index: u32,
) -> bool {
    guard(false, || {
        get(encoder)?
            .0
            .set_sampler_state(optional(sampler)?.map(|s| &s.0), index)
            .map_err(e("argument sampler"))?;
        Ok(true)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_set_compute_pipeline(
    encoder: *mut NmbArgumentEncoder,
    pipeline: *mut NmbComputePipeline,
    index: u32,
) -> bool {
    guard(false, || {
        get(encoder)?
            .0
            .set_compute_pipeline_state(optional(pipeline)?.map(|p| &p.0), index)
            .map_err(e("argument pipeline"))?;
        Ok(true)
    })
}

/// `constantDataAtIndex:`: where the program writes the constant's bytes in the argument buffer; null, with the reason, before
/// one is set.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_constant_data(encoder: *const NmbArgumentEncoder, index: u32) -> *mut c_void {
    guard(std::ptr::null_mut(), || {
        Ok(get(encoder)?
            .0
            .constant_data(index)
            .map_err(e("constant data"))?
            .as_ptr()
            .cast())
    })
}

/// `newArgumentEncoderForBufferAtIndex:`.
#[unsafe(no_mangle)]
pub extern "C" fn nmb_argument_encoder_new_nested(
    encoder: *const NmbArgumentEncoder,
    index: u32,
) -> *mut NmbArgumentEncoder {
    guard(std::ptr::null_mut(), || {
        Ok(boxed(NmbArgumentEncoder(
            get(encoder)?
                .0
                .new_argument_encoder_for_buffer_at_index(index)
                .map_err(e("nested argument encoder"))?,
        )))
    })
}
