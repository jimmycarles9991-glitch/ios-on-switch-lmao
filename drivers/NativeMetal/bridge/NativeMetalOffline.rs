// SPDX-License-Identifier: MIT
//! The direct compile with no m2v runtime: the translator's compile devices made from the build's compile profile (the
//! enabled description and policy of the runtime's device, frozen per IPSW build in compile_profile/<build>.json) instead
//! of a Vulkan query, and libraries and functions made by the translator alone. nmb_compile_profile_check holds a live
//! runtime to the profile; nmb_compile_profile_freeze writes the profile of one.

use crate::direct::RAW_HEAP_ELEMENTS;
use crate::objects::{
    DescriptorFunction, NmbComputePipelineDescriptorOf, NmbConstant, NmbDevice, NmbRenderPipelineDescriptorOf,
    attribute_out, compute_descriptor, e, get, guard, release, specialization,
};
use metal2vulkan::Budget;
use metal2vulkan::device::{
    Device, DeviceDescription, DevicePolicy, FramebufferFetchPolicy, NativeShaderTarget, VkExtension, VkFeature,
};
use metal2vulkan::library::{FunctionConstantValues, Library, LibraryType};
use metal2vulkan::pipeline::{FunctionDescriptor, LinkedFunction};
use metal2vulkan::reflection::{Attribute, FunctionProperties};
use std::ffi::{CStr, c_char, c_void};
use std::sync::OnceLock;

const PROFILE: &str = include_str!(env!("NMB_COMPILE_PROFILE"));

#[derive(serde::Serialize, serde::Deserialize)]
struct Profile {
    enabled: DeviceDescription,
    policy: DevicePolicy,
}

/// The direct compile's three devices: the default render and compute form, the barrier form (no pixel interlock), the
/// ordered form (`RequireOrdered`, centre fragment depth); and the runtime's own compile device, which its states compile
/// on
pub(crate) struct Devices {
    pub(crate) native: Device,
    pub(crate) barrier: Device,
    pub(crate) ordered: Device,
    pub(crate) runtime: Device,
}

impl Devices {
    /// The device a render state's form compiles on, as the runtime path picks it
    pub(crate) fn render(&self, barrier: bool, fragment_tests: u32) -> &Device {
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
                .with_heap_elements(RAW_HEAP_ELEMENTS)
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
            .with_heap_elements(RAW_HEAP_ELEMENTS),
        barrier: made(Device::new(barrier, p.policy))?,
        ordered: made(Device::new_with_center_fragment_depth(p.enabled.clone(), ordered))?,
        runtime,
    })
}

fn frozen() -> Result<&'static Profile, String> {
    static READ: OnceLock<Result<Profile, String>> = OnceLock::new();
    READ.get_or_init(|| serde_json::from_str(PROFILE).map_err(|e| format!("compile profile: {e}")))
        .as_ref()
        .map_err(Clone::clone)
}

/// The profile's devices, made once per process
pub(crate) fn offline_devices() -> Result<&'static Devices, String> {
    static MADE: OnceLock<Result<Devices, String>> = OnceLock::new();
    MADE.get_or_init(|| devices(frozen()?)).as_ref().map_err(Clone::clone)
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

#[unsafe(no_mangle)]
pub extern "C" fn nmb_offline_library_release(library: *mut NmbOfflineLibrary) {
    release(library)
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
    let budget = Budget::standard();
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
            .compile_compute_pipeline(&descriptor, &Budget::standard())
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
