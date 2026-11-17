//! `Function::buffer_encoders`: one decode answers every buffer index as
//! `argument_buffer_layout` answers it alone, a plain buffer of no struct type with Metal's empty
//! encoder, and an index with no buffer not at all. A function made without constant values has
//! no gated buffer; of one made with them, an index a function constant gates waits on
//! specialization alone (an M4 under macOS 27.2 answers both ways).

use super::argument_buffers::{fixture_function, function_layout};
use crate::bind::{ArgumentBufferLayout, ArgumentLayoutError, BufferEncoder};
use crate::budget::Budget;
use crate::error::Error;
use crate::library::{Function, FunctionConstantValue, FunctionConstantValues, Library};
use crate::metal::DataType;
use crate::pipeline::testing::AssembleError;

#[test]
fn every_buffer_index_answers_as_its_own_layout() {
    // (kernel, its argument buffers, its plain buffers)
    let kernels: [(&str, &[u32], &[u32]); 4] = [
        ("kA", &[0], &[1]),
        ("kH", &[2], &[0]),
        ("kAA", &[0], &[1]),
        ("kP", &[], &[0]),
    ];
    for (kernel, arguments, plain) in kernels {
        let Some(function) = fixture_function(kernel) else {
            return;
        };
        let encoders = function
            .buffer_encoders(None, &Budget::default())
            .expect("the kernel decodes");
        let declared: Vec<(u32, bool)> = encoders
            .iter()
            .map(|b| (b.index, b.argument_buffer))
            .collect();
        let mut want: Vec<(u32, bool)> = arguments.iter().map(|&i| (i, true)).collect();
        want.extend(plain.iter().map(|&i| (i, false)));
        want.sort_unstable();
        assert_eq!(declared, want, "{kernel}");
        for b in &encoders {
            let alone = function_layout(kernel, b.index).expect("assembled");
            if b.argument_buffer {
                assert_eq!(b.layout, alone, "{kernel} {}", b.index);
            } else {
                assert_eq!(
                    alone,
                    Err(ArgumentLayoutError::NoArgumentBuffer { index: b.index })
                );
            }
        }
        for index in (0..31).filter(|i| !declared.iter().any(|(d, _)| d == i)) {
            assert_eq!(
                function_layout(kernel, index).expect("assembled"),
                Err(ArgumentLayoutError::NoArgumentBuffer { index }),
                "{kernel} {index}"
            );
        }
    }
}

#[test]
fn a_plain_buffer_of_no_struct_takes_metals_empty_encoder() {
    let Some(function) = fixture_function("kP") else {
        return;
    };
    let encoders = function
        .buffer_encoders(None, &Budget::default())
        .expect("the kernel decodes");
    assert_eq!(encoders.len(), 1);
    assert!(!encoders[0].argument_buffer);
    let empty = ArgumentBufferLayout {
        encoded_length: 0,
        alignment: 1,
        members: Vec::new(),
    };
    assert_eq!(encoders[0].layout, Ok(empty));
}

#[test]
fn an_array_of_arrays_answers_as_its_layout_alone() {
    let Some(function) = fixture_function("kAA") else {
        return;
    };
    let encoders = function
        .buffer_encoders(None, &Budget::default())
        .expect("the kernel decodes");
    assert_eq!(Some(encoders[0].layout.clone()), function_layout("kAA", 0));
    assert!(encoders[0].layout.is_ok());
    assert!(encoders[1].layout.is_ok(), "its plain buffer answers too");
}

/// `kG`: a plain buffer at 0, and one at 1 that function constant 0 (`use_g`) gates; it stores the
/// constant's value, so a function made without it does not specialize.
const GATED: &str = r#"target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"

@use_g.MTL_FC_INIT_0_b = linkonce_odr hidden local_unnamed_addr addrspace(2) externally_initialized constant i8 undef, section "air.fc_initializer", align 1
@__metal_implicit_fc_pred_0 = internal addrspace(2) global i8 0, align 1
@llvm.global_ctors = appending global [1 x { i32, ptr, ptr }] [{ i32, ptr, ptr } { i32 65535, ptr @_GLOBAL__sub_I_, ptr null }]

define void @kG(ptr addrspace(1) %out, ptr addrspace(1) %g) {
  %v = load i8, ptr addrspace(2) @use_g.MTL_FC_INIT_0_b, align 1
  %w = zext i8 %v to i32
  store i32 %w, ptr addrspace(1) %out, align 4
  ret void
}

declare i8 @air.normalize_function_constant_predicate.i8(i8)

define internal void @_GLOBAL__sub_I_() section "air.static_init" {
  %1 = load i8, ptr addrspace(2) @use_g.MTL_FC_INIT_0_b, align 1
  %2 = tail call i8 @air.normalize_function_constant_predicate.i8(i8 %1)
  store i8 %2, ptr addrspace(2) @__metal_implicit_fc_pred_0, align 1
  ret void
}

!air.kernel = !{!30}
!air.function_constants = !{!10}
!air.version = !{!20}
!air.language_version = !{!21}
!1 = !{}
!5 = !{ptr addrspace(2) @__metal_implicit_fc_pred_0, !"bool", !"use_g"}
!10 = !{ptr addrspace(2) @use_g.MTL_FC_INIT_0_b, !"bool", !"use_g", i32 0}
!11 = !{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"out"}
!12 = !{i32 1, !"air.function_constant", !5, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.read_write", !"air.address_space", i32 1, !"air.arg_type_size", i32 4, !"air.arg_type_align_size", i32 4, !"air.arg_type_name", !"uint", !"air.arg_name", !"g"}
!22 = !{!11, !12}
!30 = !{ptr @kG, !1, !22}
!20 = !{i32 2, i32 8, i32 0}
!21 = !{!"Metal", i32 4, i32 0, i32 0}
"#;

fn gated() -> Option<Function> {
    let bitcode = match metal2vulkan_devtools::llvm_assemble_text(GATED) {
        Ok(bitcode) => bitcode,
        Err(AssembleError::Missing(why)) => {
            eprintln!("SKIPPED: {why}");
            return None;
        }
        Err(AssembleError::Rejected(why)) => panic!("llvm-as rejected the module: {why}"),
    };
    let library = Library::from_air(&bitcode).expect("the module decodes");
    Some(library.function("kG").expect("the kernel"))
}

fn empty() -> Result<ArgumentBufferLayout, ArgumentLayoutError> {
    Ok(ArgumentBufferLayout {
        encoded_length: 0,
        alignment: 1,
        members: Vec::new(),
    })
}

fn plain(index: u32, layout: Result<ArgumentBufferLayout, ArgumentLayoutError>) -> BufferEncoder {
    BufferEncoder {
        index,
        argument_buffer: false,
        layout,
    }
}

#[test]
fn a_gated_buffer_is_absent_unspecialized_and_waits_on_specialization_alone() {
    let Some(function) = gated() else {
        return;
    };
    let unspecialized = function
        .buffer_encoders(None, &Budget::default())
        .expect("the kernel decodes");
    assert_eq!(unspecialized, vec![plain(0, empty())], "no gated buffer");
    let none = function
        .buffer_encoders(
            Some(&FunctionConstantValues::defaults()),
            &Budget::default(),
        )
        .expect("the kernel decodes");
    assert_eq!(none.len(), 2);
    assert_eq!(none[0], plain(0, empty()), "the ungated buffer answers");
    assert!(
        matches!(
            none[1].layout,
            Err(ArgumentLayoutError::Compile(Error::NeedsInput(_)))
        ),
        "{:?}",
        none[1]
    );
    for (on, want) in [
        (1u8, vec![plain(0, empty()), plain(1, empty())]),
        (0, vec![plain(0, empty())]),
    ] {
        let values = FunctionConstantValues::defaults()
            .with_index(0, FunctionConstantValue::new(DataType::Bool, vec![on]));
        let specialized = function
            .buffer_encoders(Some(&values), &Budget::default())
            .expect("the kernel specializes");
        assert_eq!(specialized, want, "use_g {on}");
    }
}
