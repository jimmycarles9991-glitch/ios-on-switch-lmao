use crate::device::profile::{DeviceProfile, Limits};
use crate::device::requirements::{ApiVersion, VkFeature};
use crate::device::{Device, DeviceDescription, DevicePolicy};
use crate::lower::select::Plan;
use crate::pipeline::interface::PushLoc;
use crate::pipeline::sym::{PushField, ResourceKind, ResourceSlot, Stage};
use crate::pipeline::testing::{compile, link_standalone, typed_module};
use crate::spirv::interp::{run, Buffer, Config, Resources};
use metal2vulkan_devtools::llvm_assemble_text;

fn profile(robust: bool, standard_layout: bool) -> DeviceProfile {
    device(robust, standard_layout).profile().clone()
}

pub(crate) fn device(robust: bool, standard_layout: bool) -> Device {
    let mut desc = DeviceDescription::new(ApiVersion::V1_2);
    desc.limits = Limits {
        max_compute_work_group_size: [1024, 1024, 64],
        max_compute_work_group_count: [65535; 3],
        max_compute_work_group_invocations: 1024,
        max_compute_shared_memory_size: 32768,
        max_push_constants_size: 128,
        max_bound_descriptor_sets: 4,
        max_per_stage_descriptor_storage_buffers: 32,
        max_per_stage_descriptor_uniform_buffers: 32,
        max_per_stage_resources: 64,
        max_descriptor_set_storage_buffers: 32,
        max_descriptor_set_uniform_buffers: 32,
        max_storage_buffer_range: 1 << 27,
        max_uniform_buffer_range: 65536,
        min_storage_buffer_offset_alignment: 16,
        min_uniform_buffer_offset_alignment: 16,
        ..Limits::default()
    };
    if robust {
        desc.features.insert(VkFeature::RobustBufferAccess);
    }
    if standard_layout {
        desc.features.insert(VkFeature::UniformBufferStandardLayout);
    }
    Device::new(desc, DevicePolicy::default()).expect("test device")
}

pub(crate) fn fixture(constant: bool) -> Vec<u8> {
    let space = if constant { 2 } else { 1 };
    let text = format!(
        r#"
target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"
define void @copy_vector(ptr addrspace({space}) noundef "air-buffer-no-alias" %source, ptr addrspace(1) noundef "air-buffer-no-alias" %target) {{
  %value = load <4 x i32>, ptr addrspace({space}) %source, align 16
  store <4 x i32> %value, ptr addrspace(1) %target, align 16
  ret void
}}
!air.kernel = !{{!9}}
!air.version = !{{!17}}
!air.language_version = !{{!18}}
!9 = !{{ptr @copy_vector, !10, !11}}
!10 = !{{}}
!11 = !{{!12, !13}}
!12 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 {space}, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"source"}}
!13 = !{{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 16, !"air.arg_type_align_size", i32 16, !"air.arg_type_name", !"uint4", !"air.arg_name", !"target"}}
!17 = !{{i32 2, i32 8, i32 0}}
!18 = !{{!"Metal", i32 4, i32 0, i32 0}}
"#
    );
    llvm_assemble_text(&text).expect("the test requires its declared llvm-as tool")
}

fn push(plan: &Plan, offsets: [u32; 2]) -> Vec<u8> {
    let mut bytes = vec![0; 128];
    let mut found = [false; 2];
    for field in &plan.push.fields {
        if let (PushField::SlotBindOffset(slot), PushLoc::Push { offset }) =
            (field.field, field.loc)
        {
            let index = slot.index as usize;
            let at = offset as usize;
            bytes[at..at + 4].copy_from_slice(&offsets[index].to_le_bytes());
            found[index] = true;
        }
    }
    assert_eq!(found, [true; 2]);
    bytes
}

fn buffer(index: u32, bytes: Vec<u8>) -> Buffer {
    Buffer {
        slot: Some(ResourceSlot {
            stage: Stage::Kernel,
            kind: ResourceKind::Buffer,
            index,
        }),
        address: None,
        bytes,
    }
}

fn pattern(len: usize) -> Vec<u8> {
    (0..len).map(|n| ((n * 73 + 29) & 255) as u8).collect()
}

#[test]
fn binding_residual_preserves_vector_bytes_and_neighbors() {
    for constant in [false, true] {
        let bitcode = fixture(constant);
        for (robust, standard_layout) in [(false, false), (true, false), (true, true)] {
            let profile = profile(robust, standard_layout);
            let (plan, module) =
                typed_module(&bitcode, "copy_vector", &profile).expect("typed module");
            let artifact = compile(&bitcode, "copy_vector", &profile).expect("SPIR-V artifact");
            link_standalone(&artifact, &profile).expect("linked artifact");
            for source_offset in [0, 4, 8, 12, 16] {
                for target_offset in [0, 4, 8, 12, 16] {
                    let source = pattern(64);
                    let target = vec![0xa5; 64];
                    let mut expected = target.clone();
                    expected[target_offset as usize..target_offset as usize + 16].copy_from_slice(
                        &source[source_offset as usize..source_offset as usize + 16],
                    );
                    let result = run(
                        &module,
                        &Config::default(),
                        Resources {
                            buffers: vec![buffer(0, source.clone()), buffer(1, target)],
                            push: push(&plan, [source_offset, target_offset]),
                            spec: Default::default(),
                        },
                    )
                    .expect("execute actual emitted module");
                    assert_eq!(result.buffers[0], source);
                    assert_eq!(result.buffers[1], expected,
                        "constant={constant} robust={robust} standard={standard_layout} source={source_offset} target={target_offset}");
                }
            }
        }
    }
}

#[test]
fn truncated_ranges_contain_the_entire_vector_access() {
    let bitcode = fixture(false);
    let profile = profile(false, false);
    let (plan, module) = typed_module(&bitcode, "copy_vector", &profile).expect("typed module");
    for source_len in [16, 32, 48] {
        for target_len in [16, 32, 48] {
            for source_offset in [0, 4, 8, 12, 16] {
                for target_offset in [0, 4, 8, 12, 16] {
                    let source = pattern(source_len);
                    let target = vec![0xa5; target_len];
                    let mut expected = target.clone();
                    if target_offset as usize + 16 <= target_len {
                        let target =
                            &mut expected[target_offset as usize..target_offset as usize + 16];
                        if source_offset as usize + 16 <= source_len {
                            target.copy_from_slice(
                                &source[source_offset as usize..source_offset as usize + 16],
                            );
                        } else {
                            target.fill(0);
                        }
                    }
                    let result = run(
                        &module,
                        &Config::default(),
                        Resources {
                            buffers: vec![buffer(0, source.clone()), buffer(1, target)],
                            push: push(&plan, [source_offset, target_offset]),
                            spec: Default::default(),
                        },
                    )
                    .expect("contained execution");
                    assert_eq!(result.buffers[0], source);
                    assert_eq!(
                        result.buffers[1], expected,
                        "source={source_len}@{source_offset} target={target_len}@{target_offset}"
                    );
                }
            }
        }
    }
}
