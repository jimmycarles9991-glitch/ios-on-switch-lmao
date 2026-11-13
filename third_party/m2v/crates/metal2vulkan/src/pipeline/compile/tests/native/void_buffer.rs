use super::*;
use crate::metal::{BindingAccess, DataType};
use crate::reflection::Binding;
use metal2vulkan_devtools::llvm_assemble_text;

fn function(name: &str, layout: &str) -> crate::library::Function {
    let text = format!(
        r#"target datalayout = "e-p:64:64:64-i1:8:8-i8:8:8-i16:16:16-i32:32:32-i64:64:64-f32:32:32-f64:64:64-v16:16:16-v24:32:32-v32:32:32-v48:64:64-v64:64:64-v96:128:128-v128:128:128-v192:256:256-v256:256:256-v512:512:512-v1024:1024:1024-n8:16:32"
target triple = "air64_v28-apple-macosx26.0.0"
define void @untyped_buffer(ptr addrspace(2) %source, ptr addrspace(1) %target) {{
  %value = load i32, ptr addrspace(2) %source, align 4
  store i32 %value, ptr addrspace(1) %target, align 4
  ret void
}}
!air.kernel = !{{!0}}
!air.version = !{{!4}}
!air.language_version = !{{!5}}
!0 = !{{ptr @untyped_buffer, !1, !2}}
!1 = !{{}}
!2 = !{{!3, !6}}
!3 = !{{i32 0, !"air.buffer", !"air.location_index", i32 0, i32 1, !"air.read", !"air.address_space", i32 2, {layout}!"air.arg_type_name", !"{name}"}}
!4 = !{{i32 2, i32 8, i32 0}}
!5 = !{{!"Metal", i32 4, i32 0, i32 0}}
!6 = !{{i32 1, !"air.buffer", !"air.location_index", i32 1, i32 1, !"air.write", !"air.address_space", i32 1, !"air.arg_type_name", !"void"}}
"#
    );
    let bytes = llvm_assemble_text(&text).expect("assemble void-buffer AIR");
    Library::from_air(&bytes)
        .unwrap()
        .function("untyped_buffer")
        .unwrap()
}

fn assert_binding(binding: &Binding, index: u32, access: BindingAccess) {
    assert_eq!(binding.name(), "");
    assert_eq!(binding.index(), index);
    assert_eq!(binding.access(), access);
    assert!(binding.is_used());
    let buffer = binding.buffer().unwrap();
    assert_eq!(buffer.buffer_data_type(), DataType::None);
    assert_eq!(buffer.buffer_data_size(), 0);
    assert_eq!(buffer.buffer_alignment(), 4);
    assert!(buffer.buffer_struct_type().is_none());
    let pointer = buffer.buffer_pointer_type().unwrap();
    assert_eq!(pointer.element_type(), DataType::None);
    assert_eq!(pointer.data_size(), 0);
    assert_eq!(pointer.alignment(), 4);
    assert_eq!(pointer.access(), access);
    assert!(!pointer.element_is_argument_buffer());
}

#[test]
fn generated_void_buffers_match_metal_and_survive_native_artifacts() {
    let f = function("void", "");
    let budget = Budget::standard();
    let dev = device(true, false);
    let descriptor = kernel(f.clone());
    let library = f.reflection(&budget).unwrap().unwrap();
    let pipeline = descriptor.reflection(&budget).unwrap();
    let native = dev
        .compile_native_function(&FunctionDescriptor::new(f), &LoweringInputs::new(), &budget)
        .unwrap()
        .into_pipeline(&dev, &descriptor)
        .unwrap();
    let saved = native.reflection(&descriptor).unwrap();
    for bindings in [library.bindings(), pipeline.bindings(), saved.bindings()] {
        assert_eq!(bindings.len(), 2);
        assert_binding(&bindings[0], 0, BindingAccess::ReadOnly);
        assert_binding(&bindings[1], 1, BindingAccess::WriteOnly);
    }
    assert!(!native.portable_is_materialized());
}

#[test]
fn unknown_names_and_partial_layouts_are_still_refused() {
    for (name, layout) in [
        ("unrecognized", ""),
        ("void", r#"!"air.arg_type_size", i32 0, "#),
        ("void", r#"!"air.arg_type_align_size", i32 4, "#),
        ("float", ""),
    ] {
        let f = function(name, layout);
        assert!(
            f.reflection(&Budget::standard()).is_err(),
            "{name}: {layout}"
        );
    }
}
