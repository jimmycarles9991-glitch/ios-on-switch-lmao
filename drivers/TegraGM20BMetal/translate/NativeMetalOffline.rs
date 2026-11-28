// SPDX-License-Identifier: MIT
//! The direct compile with no m2v runtime: the translator's compile devices made from the build's compile profile (the
//! enabled description and policy of the runtime's device, frozen per IPSW build in compile_profile/<build>.json) instead
//! of a Vulkan query, and libraries and functions made by the translator alone. The bridge's nmb_compile_profile_check
//! holds a live runtime to the profile; its nmb_compile_profile_freeze writes the profile of one.

use crate::argument_layouts::{NmbArgumentLayout, with_flat_layouts};
use crate::descriptors::{
    DescriptorFunction, NmbComputePipelineDescriptorOf, NmbConstant, NmbRenderPipelineDescriptorOf, attribute_out,
    compute_descriptor, e, get, guard, release, specialization,
};
use crate::direct_abi::SWITCH_METAL_HEAP_ELEMENTS;
use metal2vulkan::device::{
    Device, DeviceDescription, DevicePolicy, FramebufferFetchPolicy, NativeShaderTarget, VkExtension, VkFeature,
};
use metal2vulkan::library::{FunctionConstantValues, HeldBytes, Library, LibraryType};
use metal2vulkan::pipeline::{FunctionDescriptor, LinkedFunction};
use metal2vulkan::reflection::{Attribute, FunctionProperties};
use std::ffi::{CStr, CString, c_char, c_void};
use std::sync::{Arc, OnceLock};

const PROFILE: &str = include_str!(env!("NMB_COMPILE_PROFILE"));

#[derive(serde::Serialize, serde::Deserialize)]
pub struct Profile {
    pub enabled: DeviceDescription,
    pub policy: DevicePolicy,
}

/// The direct compile's three devices: the default render and compute form, the barrier form (no pixel interlock), the
/// ordered form (`RequireOrdered`, centre fragment depth); and the runtime's own compile device, which its states compile
/// on
pub struct Devices {
    pub native: Device,
    pub barrier: Device,
    pub ordered: Device,
    pub runtime: Device,
}

impl Devices {
    /// The device a render state's form compiles on, as the runtime path picks it
    pub fn render(&self, barrier: bool, fragment_tests: u32) -> &Device {
        if barrier {
            &self.barrier
        } else if fragment_tests != 0 {
            &self.ordered
        } else {
            &self.native
        }
    }
}

fn devices(p: &Profile) -> Result<Devices, String> {
    let made = |d: Result<Device, metal2vulkan::error::Error>| {
        d.map(|d| {
            d.with_native_target(NativeShaderTarget::Sm53)
                .with_heap_elements(SWITCH_METAL_HEAP_ELEMENTS)
        })
        .map_err(e("compile device"))
    };
    let mut barrier = p.enabled.clone();
    barrier.extensions.remove(&VkExtension::ExtFragmentShaderInterlock);
    barrier.features.remove(&VkFeature::FragmentShaderPixelInterlock);
    barrier.features.remove(&VkFeature::FragmentShaderSampleInterlock);
    let mut ordered = p.policy;
    ordered.framebuffer_fetch = FramebufferFetchPolicy::RequireOrdered;
    let runtime = Device::new(p.enabled.clone(), p.policy).map_err(e("compile device"))?;
    Ok(Devices {
        native: runtime
            .with_native_target(NativeShaderTarget::Sm53)
            .with_heap_elements(SWITCH_METAL_HEAP_ELEMENTS),
        barrier: made(Device::new(barrier, p.policy))?,
        ordered: made(Device::new_with_center_fragment_depth(p.enabled.clone(), ordered))?,
        runtime,
    })
}

pub fn frozen() -> Result<&'static Profile, String> {
    static READ: OnceLock<Result<Profile, String>> = OnceLock::new();
    READ.get_or_init(|| serde_json::from_str(PROFILE).map_err(|e| format!("compile profile: {e}")))
        .as_ref()
        .map_err(Clone::clone)
}

/// The profile's devices, made once per process
pub fn offline_devices() -> Result<&'static Devices, String> {
    static MADE: OnceLock<Result<Devices, String>> = OnceLock::new();
    MADE.get_or_init(|| devices(frozen()?)).as_ref().map_err(Clone::clone)
}

/// A metallib's library, made by the translator alone
pub struct NmbOfflineLibrary(Library);

/// A function of an offline library as the runtime's `Function` holds it: with the constant values it was made with,
/// whether it was made with constant values at all (its properties are then the specialized function's), and the name a
/// pipeline links it under. Never precompiled: no runtime makes it a binary.
pub struct NmbOfflineFunction {
    function: metal2vulkan::library::Function,
    constants: FunctionConstantValues,
    specialized: bool,
    link_name: Option<String>,
}

pub type NmbOfflineRenderPipelineDescriptor = NmbRenderPipelineDescriptorOf<NmbOfflineFunction>;
pub type NmbOfflineComputePipelineDescriptor = NmbComputePipelineDescriptorOf<NmbOfflineFunction>;

impl DescriptorFunction for NmbOfflineFunction {
    fn stage_function(&self) -> FunctionDescriptor {
        let mut descriptor = FunctionDescriptor::new(self.function.clone());
        descriptor.constants = self.constants.clone();
        descriptor
    }

    fn linked_function(&self) -> LinkedFunction {
        LinkedFunction::new(self.function.clone())
            .with_constants(self.constants.clone())
            .with_precompiled(false)
            .with_link_name(self.link_name.clone())
    }
}

/// The library of a metallib (a universal one gives its AIR64 slice), as nmb_library_new makes it
///
/// # Safety
/// `bytes` holds `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_library_new(bytes: *const c_void, length: usize) -> *mut NmbOfflineLibrary {
    guard(std::ptr::null_mut(), || {
        if bytes.is_null() || length == 0 {
            return Err("no library bytes".into());
        }
        // SAFETY: the caller's contract.
        let bytes = unsafe { std::slice::from_raw_parts(bytes.cast::<u8>(), length) };
        let library = Library::from_metallib(crate::air_slice(bytes).ok_or("metallib without an AIR64 slice")?)
            .map_err(e("metallib"))?;
        Ok(Box::into_raw(Box::new(NmbOfflineLibrary(library))))
    })
}

/// Bytes a library's caller holds for it: given back through `release` once the library's last clone is gone
struct Held {
    bytes: *const u8,
    length: usize,
    owner: *const c_void,
    release: unsafe extern "C" fn(*const c_void),
}

// SAFETY: the caller's bytes do not change while they are held, and its `release` runs on any thread
// (nmb_offline_library_new_held).
unsafe impl Send for Held {}
unsafe impl Sync for Held {}

impl std::fmt::Debug for Held {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        write!(f, "{} held bytes", self.length)
    }
}

impl HeldBytes for Held {
    fn bytes(&self) -> &[u8] {
        if self.bytes.is_null() {
            return &[];
        }
        // SAFETY: the caller's contract.
        unsafe { std::slice::from_raw_parts(self.bytes, self.length) }
    }
}

impl Drop for Held {
    fn drop(&mut self) {
        // SAFETY: the caller's contract.
        unsafe { (self.release)(self.owner) }
    }
}

/// nmb_offline_library_new's library over bytes the caller holds, with no copy, its functions' HASH tags taken as
/// stated, so only its function list is read until a function of it is compiled. `scope`: 0 for a library read from a
/// sealed system volume, else one nmb_offline_scope_new gave.
///
/// # Safety
/// `bytes` holds `length` bytes, unchanged until `release(owner)`, which is called once, on any thread, also when no
/// library is made.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_library_new_held(
    bytes: *const c_void,
    length: usize,
    scope: u64,
    owner: *const c_void,
    release: unsafe extern "C" fn(*const c_void),
) -> *mut NmbOfflineLibrary {
    let held: Arc<dyn HeldBytes> = Arc::new(Held {
        bytes: bytes.cast(),
        length,
        owner,
        release,
    });
    guard(std::ptr::null_mut(), move || {
        if held.bytes().is_empty() {
            return Err("no library bytes".into());
        }
        let library = match scope {
            0 => Library::from_held_metallib(held, true),
            scope => Library::from_held_metallib_in(held, scope),
        }
        .map_err(e("metallib"))?;
        Ok(Box::into_raw(Box::new(NmbOfflineLibrary(library))))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_scope_new() -> u64 {
    Library::new_scope()
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_library_release(library: *mut NmbOfflineLibrary) {
    release(library)
}

/// The system shader compiler's: keep up to `bytes` of each kind of compiled stage, so a stage another state compiles
/// under the same inputs is translated once (metal2vulkan::pipeline::keep_compiled_stages)
#[unsafe(no_mangle)]
pub extern "C" fn nmb_keep_translations(bytes: u64) {
    metal2vulkan::pipeline::keep_compiled_stages(usize::try_from(bytes).unwrap_or(usize::MAX));
}

/// The kept stages into `out`'s two words: entries and bytes
///
/// # Safety
/// `out` holds two words.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_kept_translations(out: *mut u64) {
    if out.is_null() {
        return;
    }
    let k = metal2vulkan::pipeline::kept_compiled_stages();
    // SAFETY: the caller's two words.
    unsafe { std::slice::from_raw_parts_mut(out, 2) }.copy_from_slice(&[k.entries, k.bytes]);
}

fn offline_function(
    library: *const NmbOfflineLibrary,
    name: &str,
    constants: FunctionConstantValues,
    specialized: bool,
    link_name: Option<String>,
) -> Result<*mut NmbOfflineFunction, String> {
    let library = &get(library)?.0;
    if library.library_type() == LibraryType::Dynamic {
        return Err("a dynamic library's function".into());
    }
    let function = library.function(name).map_err(e("function"))?;
    Ok(Box::into_raw(Box::new(NmbOfflineFunction {
        function,
        constants,
        specialized,
        link_name,
    })))
}

/// nmb_function_new's function, of an offline library
///
/// # Safety
/// `name` is a C string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_function_new(
    library: *const NmbOfflineLibrary,
    name: *const c_char,
) -> *mut NmbOfflineFunction {
    guard(std::ptr::null_mut(), || {
        if name.is_null() {
            return Err("null function name".into());
        }
        // SAFETY: the caller's contract.
        let name = unsafe { CStr::from_ptr(name) }.to_str().map_err(e("function name"))?;
        offline_function(library, name, FunctionConstantValues::defaults(), false, None)
    })
}

/// nmb_function_new_specialized's function, of an offline library
///
/// # Safety
/// As nmb_function_new_specialized's.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_function_new_specialized(
    library: *const NmbOfflineLibrary,
    name: *const c_char,
    specialized_name: *const c_char,
    constants: *const NmbConstant,
    count: u32,
) -> *mut NmbOfflineFunction {
    guard(std::ptr::null_mut(), || {
        // SAFETY: the caller's contract.
        let (name, values, link_name) = unsafe { specialization(name, specialized_name, constants, count) }?;
        offline_function(library, name, values, true, link_name)
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_release(function: *mut NmbOfflineFunction) {
    release(function)
}

/// The function's `MTLFunction` properties as the runtime's function answers them, failing with `what`
fn properties(function: *const NmbOfflineFunction, what: &'static str) -> Result<FunctionProperties, String> {
    let f = get(function)?;
    let budget = crate::compile_budget();
    if f.specialized {
        f.function.specialized_properties(&f.constants, &budget)
    } else {
        f.function.properties(&budget)
    }
    .map_err(e(what))
}

/// nmb_function_type's answer, of an offline function
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_type(function: *const NmbOfflineFunction) -> u64 {
    guard(0, || {
        Ok(properties(function, "function type")?.function_type().as_raw() as u64)
    })
}

/// Whether the runtime's compile device compiles `descriptor`'s kernel, as its compute state creation does
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_compute_pipeline_compiles(
    descriptor: *const NmbOfflineComputePipelineDescriptor,
) -> bool {
    guard(false, || {
        let descriptor = compute_descriptor(descriptor)?;
        offline_devices()?
            .runtime
            .compile_compute_pipeline(&descriptor, &crate::compile_budget())
            .map_err(e("compute pipeline"))?;
        Ok(true)
    })
}

/// `vertexAttributes` as the runtime's function answers it: `None` where Metal answers nil
fn vertex_attributes(function: *const NmbOfflineFunction) -> Result<Option<Vec<Attribute>>, String> {
    Ok(properties(function, "vertex attributes")?
        .vertex_attributes()
        .map(<[Attribute]>::to_vec))
}

/// nmb_function_attribute_count's answer for `vertexAttributes`, of an offline function
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_vertex_attribute_count(function: *const NmbOfflineFunction) -> i64 {
    guard(-1, || Ok(vertex_attributes(function)?.map_or(-1, |a| a.len() as i64)))
}

/// nmb_function_attribute's answer for `vertexAttributes`, of an offline function
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_vertex_attribute(
    function: *const NmbOfflineFunction,
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> usize {
    guard(0, || {
        attribute_out(&vertex_attributes(function)?.unwrap_or_default(), at, out, size, info)
    })
}

/// `stageInputAttributes` as the runtime's function answers it: `None` where Metal answers nil
fn stage_input_attributes(function: *const NmbOfflineFunction) -> Result<Option<Vec<Attribute>>, String> {
    Ok(properties(function, "stage input attributes")?
        .stage_input_attributes()
        .map(<[Attribute]>::to_vec))
}

/// nmb_function_attribute_count's answer for `stageInputAttributes`, of an offline function
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_stage_input_attribute_count(function: *const NmbOfflineFunction) -> i64 {
    guard(-1, || {
        Ok(stage_input_attributes(function)?.map_or(-1, |a| a.len() as i64))
    })
}

/// nmb_function_attribute's answer for `stageInputAttributes`, of an offline function
#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_function_stage_input_attribute(
    function: *const NmbOfflineFunction,
    at: u32,
    out: *mut c_char,
    size: usize,
    info: *mut u64,
) -> usize {
    guard(0, || {
        attribute_out(
            &stage_input_attributes(function)?.unwrap_or_default(),
            at,
            out,
            size,
            info,
        )
    })
}

/// What `newArgumentEncoderWithBufferIndex:` encodes into at every buffer index `function` declares, in index order
/// (metal2vulkan bind BufferEncoder): `each` gets the index, whether it is an argument buffer (else a plain buffer, whose
/// encoder Metal makes only without API validation), and its layouts as nmb_function_argument_layouts gives them, or none
/// and `refusal` where Metal makes no encoder. False with nmb_last_error where the function does not decode, or `each`'s
/// false.
///
/// # Safety
/// `each` is valid for `ctx` for the call's length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn nmb_offline_function_buffer_encoders(
    function: *const NmbOfflineFunction,
    each: unsafe extern "C" fn(
        index: u32,
        argument_buffer: bool,
        layouts: *const NmbArgumentLayout,
        count: u32,
        refusal: *const c_char,
        ctx: *mut c_void,
    ) -> bool,
    ctx: *mut c_void,
) -> bool {
    guard(false, || {
        let f = get(function)?;
        let encoders = f
            .function
            .buffer_encoders(f.specialized.then_some(&f.constants), &crate::compile_budget())
            .map_err(e("buffer encoders"))?;
        for b in &encoders {
            // SAFETY: the caller's contract; the layouts and the refusal live until after the call.
            let going = match &b.layout {
                Ok(layout) => with_flat_layouts(layout, |l| unsafe {
                    each(
                        b.index,
                        b.argument_buffer,
                        l.as_ptr(),
                        l.len() as u32,
                        std::ptr::null(),
                        ctx,
                    )
                })?,
                Err(why) => {
                    let refusal = CString::new(format!("{why:?}")).map_err(e("refusal"))?;
                    unsafe { each(b.index, b.argument_buffer, std::ptr::null(), 0, refusal.as_ptr(), ctx) }
                }
            };
            if !going {
                return Ok(false);
            }
        }
        Ok(true)
    })
}
