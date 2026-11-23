//! Modules the front end closed, linked into a kernel as MPS links its kernels (MPSKernelDAG):
//! a stitched library's function (`air.stitching_modules_signature`) and the front end's
//! specialization of a visible function (`air.linked_constants_signature` wrapped in a rename to
//! its specialized name), which calls the stitched one. `STITCHED` is what Apple's front end wrote
//! (macOS 27.2) for HW Test's shaders/Stitching.metal graph `stitch_scaled` over `stitch_scaled_add`
//! specialized with `stitch_gain` 2 (gpu.gm20b.stitched_library); `SPECIALIZED` is the shape of
//! MPS's `prefixPrimary_f` over its stitched `prefixPrimary_i8`.

use super::*;
use crate::pipeline::descriptor::LinkedList;

const HEADER: &str = "target datalayout = \"e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32\"\ntarget triple = \"air64_v29-apple-macosx27.2.0\"\n\n";

/// `out[tid] = stitch_scaled(float(tid), 3)` and `out[tid] = prefix_f(float(tid), 3)`.
const CONSUMER: &str = r#"
define void @stitch_consumer(ptr addrspace(1) noundef "air-buffer-no-alias" %0, i32 noundef %1) local_unnamed_addr {
  %3 = tail call fast float @air.convert.f.f32.u.i32(i32 %1)
  %4 = tail call fast float @stitch_scaled.MTL_VISIBLE_FN_REF(float noundef %3, float noundef 3.000000e+00)
  %5 = zext i32 %1 to i64
  %6 = getelementptr inbounds float, ptr addrspace(1) %0, i64 %5
  store float %4, ptr addrspace(1) %6, align 4
  ret void
}

define void @specialized_consumer(ptr addrspace(1) noundef "air-buffer-no-alias" %0, i32 noundef %1) local_unnamed_addr {
  %3 = tail call fast float @air.convert.f.f32.u.i32(i32 %1)
  %4 = tail call fast float @prefix_f.MTL_VISIBLE_FN_REF(float noundef %3, float noundef 3.000000e+00)
  %5 = zext i32 %1 to i64
  %6 = getelementptr inbounds float, ptr addrspace(1) %0, i64 %5
  store float %4, ptr addrspace(1) %6, align 4
  ret void
}

declare float @stitch_scaled.MTL_VISIBLE_FN_REF(float, float) local_unnamed_addr section "air.externally_defined"

declare float @prefix_f.MTL_VISIBLE_FN_REF(float, float) local_unnamed_addr section "air.externally_defined"

declare float @air.convert.f.f32.u.i32(i32)

!air.kernel = !{!0, !5}
!air.visible_function_references = !{!6, !7}
!air.version = !{!8}
!air.language_version = !{!9}

!0 = !{ptr @stitch_consumer, !1, !2}
!1 = !{}
!2 = !{!3, !4}
!3 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"float", !"air.arg_name", !"out"}
!4 = !{i32 1, !"air.thread_position_in_grid", !"air.arg_type_name", !"uint", !"air.arg_name", !"tid"}
!5 = !{ptr @specialized_consumer, !1, !2}
!6 = !{!"air.visible_function_reference", ptr @stitch_scaled.MTL_VISIBLE_FN_REF, !"stitch_scaled"}
!7 = !{!"air.visible_function_reference", ptr @prefix_f.MTL_VISIBLE_FN_REF, !"prefix_f"}
!8 = !{i32 2, i32 9, i32 0}
!9 = !{!"Metal", i32 4, i32 1, i32 0}
"#;

/// `stitch_scaled(a, b) = a * 2 + b`, the stitched node inlined, with the stitching helpers
const STITCHED: &str = r#"
@llvm.compiler.used = appending global [4 x ptr] [ptr @load_argument, ptr @copy_from_buffer, ptr @store_return_value, ptr @destroy], section "llvm.metadata"

define internal float @load_argument(ptr noundef readonly %ptr) {
entry:
  %0 = load float, ptr %ptr, align 4
  ret float %0
}

define internal void @copy_from_buffer(ptr noundef writeonly %ptr, ptr addrspace(2) noundef readonly %buffer_ptr) {
entry:
  %0 = load float, ptr addrspace(2) %buffer_ptr, align 4
  store float %0, ptr %ptr, align 4
  ret void
}

define internal void @store_return_value(ptr noundef writeonly %ptr, float noundef %arg) {
entry:
  store float %arg, ptr %ptr, align 4
  ret void
}

define internal void @destroy(ptr %ptr) {
entry:
  ret void
}

define float @stitch_scaled(float %input0.0, float %input1.0) local_unnamed_addr {
entry:
  %mul.i = fmul fast float %input0.0, 2.000000e+00
  %add.i = fadd fast float %mul.i, %input1.0
  ret float %add.i
}

!air.version = !{!8}
!air.language_version = !{!10}
!air.visible = !{!14}
!air.signature = !{!24}

!8 = !{i32 2, i32 9, i32 0}
!10 = !{!"Metal", i32 4, i32 1, i32 0}
!14 = !{ptr @stitch_scaled, !15, !17, !20}
!15 = !{!16}
!16 = !{!"air.visible_output", !"air.arg_type_name", !"float"}
!17 = !{!18, !19}
!18 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float"}
!19 = !{i32 1, !"air.visible_input", !"air.arg_type_name", !"float"}
!20 = !{!"air.stitching_info", !21, !23, !23}
!21 = !{!"air.stitching_type", !22, ptr @load_argument, ptr @store_return_value, ptr @destroy, ptr @copy_from_buffer}
!22 = !{!"air.float_type", i32 4, i32 4, null}
!23 = !{!"air.stitching_argument", !21}
!24 = !{!"air.stitching_modules_signature", !25, !31}
!25 = !{!26, !28, !29}
!26 = !{!"air.stitching_input_node", i32 0, !27, !27}
!27 = !{}
!28 = !{!"air.stitching_input_node", i32 1, !27, !27}
!29 = !{!"air.stitching_function_node", !"node_output", i1 true, !"stitch_scaled_add_2", !30, !27}
!30 = !{!26, !28}
!31 = !{!32}
!32 = !{!"air.rename_qualified_function_signature", !33, !"stitch_scaled_add_2"}
!33 = !{!"air.linked_constants_signature", !34, !35}
!34 = !{!"air.module_signature", !"7E13C8BE745316C1EDE8F153F2249FD7CD8C60A2931938F688ACC0AD6D9B5E0C"}
!35 = !{i32 2, !"uint", i32 3}
"#;

/// `prefix_f(a, b) = stitch_scaled(a, b) + 1`, specialized and renamed by the front end
const SPECIALIZED: &str = r#"
define float @prefix_f(float noundef %0, float noundef %1) local_unnamed_addr {
  %3 = tail call fast float @stitch_scaled.MTL_VISIBLE_FN_REF(float noundef %0, float noundef %1)
  %4 = fadd fast float %3, 1.000000e+00
  ret float %4
}

declare float @stitch_scaled.MTL_VISIBLE_FN_REF(float, float) local_unnamed_addr section "air.externally_defined"

!air.version = !{!0}
!air.language_version = !{!1}
!air.visible = !{!2}
!air.visible_function_references = !{!7}
!air.signature = !{!8}

!0 = !{i32 2, i32 9, i32 0}
!1 = !{!"Metal", i32 4, i32 1, i32 0}
!2 = !{ptr @prefix_f, !3, !5}
!3 = !{!4}
!4 = !{!"air.visible_output", !"air.arg_type_name", !"float"}
!5 = !{!6, !12}
!6 = !{i32 0, !"air.visible_input", !"air.arg_type_name", !"float", !"air.arg_name", !"a"}
!12 = !{i32 1, !"air.visible_input", !"air.arg_type_name", !"float", !"air.arg_name", !"b"}
!7 = !{!"air.visible_function_reference", ptr @stitch_scaled.MTL_VISIBLE_FN_REF, !"stitch_scaled"}
!8 = !{!"air.rename_qualified_function_signature", !9, !"prefix_f"}
!9 = !{!"air.linked_constants_signature", !10, !11}
!10 = !{!"air.module_signature", !"00112233445566778899AABBCCDDEEFF00112233445566778899AABBCCDDEEFF"}
!11 = !{i32 2, !"uint", i32 3}
"#;

fn assembled(name: &str, body: &str) -> Vec<u8> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let dir = std::env::temp_dir().join(format!(
        "m2v-signed-{}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    std::fs::create_dir_all(&dir).expect("scratch");
    let ll = dir.join(format!("{name}.ll"));
    std::fs::write(&ll, format!("{HEADER}{body}")).expect("write");
    let bitcode = assemble_ll(name, &ll);
    let _ = std::fs::remove_dir_all(&dir);
    bitcode.expect("llvm-as is required to link the front end's closed modules")
}

/// Each consumer's words on the CPU interpreter: `tid * 2 + offset`.
#[test]
fn the_front_ends_closed_modules_link_into_a_kernel_and_compute_metals_words() {
    let consumer = assembled("consumer", CONSUMER);
    let stitched = assembled("stitched", STITCHED);
    let specialized = assembled("specialized", SPECIALIZED);
    let none = FunctionConstantValues::defaults();
    let linked = [
        LinkedSource::new("stitch_scaled", &stitched, &none),
        LinkedSource::new("prefix_f", &specialized, &none),
    ]
    .map(|mut source| {
        source.key.list = LinkedList::PrivateFunctions;
        source
    });
    let p = profile("moltenvk-m3", DevicePolicy::default()).expect("profile");
    const N: u32 = 32;
    for (entry, offset) in [("stitch_consumer", 3.0f32), ("specialized_consumer", 4.0)] {
        let (_, module) = typed_module_linked(&consumer, entry, &p, &linked)
            .unwrap_or_else(|e| panic!("{entry}: {e}"));
        let out = run(
            &module,
            &Config {
                workgroups: [N, 1, 1],
                ..Config::default()
            },
            Resources {
                buffers: vec![Buffer {
                    slot: Some(ResourceSlot {
                        stage: Stage::Kernel,
                        kind: ResourceKind::Buffer,
                        index: 0,
                    }),
                    address: None,
                    bytes: vec![0; N as usize * 4],
                }],
                push: vec![0; 128],
                spec: Default::default(),
            },
        )
        .unwrap_or_else(|e| panic!("{entry}: {e:?}"));
        let want: Vec<u8> = (0..N)
            .flat_map(|i| (i as f32 * 2.0 + offset).to_le_bytes())
            .collect();
        assert_eq!(out.buffers[0], want, "{entry}");
    }
}

/// Through the public API, as the shader compiler makes a kernel's state: both modules as private
/// functions of the specialized consumer.
#[test]
fn a_compute_pipeline_links_the_front_ends_closed_modules_as_private_functions() {
    let consumer = Library::from_air(&assembled("consumer", CONSUMER)).expect("consumer");
    let stitched = Library::from_air(&assembled("stitched", STITCHED)).expect("stitched");
    let specialized =
        Library::from_air(&assembled("specialized", SPECIALIZED)).expect("specialized");
    let linked = LinkedFunctions::new()
        .with_private_function(specialized.function("prefix_f").expect("prefix_f"))
        .with_private_function(stitched.function("stitch_scaled").expect("stitch_scaled"));
    let descriptor = ComputePipelineDescriptor::new(StageSource::Function(
        FunctionDescriptor::new(consumer.function("specialized_consumer").expect("entry"))
            .with_linked(linked),
    ));
    device("moltenvk-m3")
        .compile_compute_pipeline(&descriptor, &Budget::standard())
        .unwrap_or_else(|e| panic!("{e}"));
}
