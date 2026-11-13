use super::*;
use crate::tests::engine_bound_alignment::{device, fixture};
use std::sync::Arc;

mod void_buffer;

#[test]
fn native_metadata_and_lazy_portable_words_match_existing_pipeline() {
    let bc = fixture(false);
    let f = Library::from_air(&bc)
        .unwrap()
        .function("copy_vector")
        .unwrap();
    for (robust, standard_layout) in [(false, false), (true, false), (true, true)] {
        let dev = device(robust, standard_layout);
        let fd = FunctionDescriptor::new(f.clone());
        let mut descriptor = kernel(f.clone());
        descriptor.max_total_threads_per_threadgroup = Some(64);
        descriptor.support_indirect_command_buffers = true;
        let native = dev
            .compile_native_function(&fd, &LoweringInputs::new(), &Budget::standard())
            .unwrap()
            .into_pipeline(&dev, &descriptor)
            .unwrap();
        let portable = dev
            .compile_compute_pipeline(&descriptor, &Budget::standard())
            .unwrap();
        assert!(!native.portable_is_materialized());
        assert!(native.metadata.stage.spirv.is_empty());
        let mut metadata = portable.clone();
        metadata.stage.spirv = Arc::from([]);
        assert_eq!(native.metadata, metadata);
        assert_eq!(
            native.reflection(&descriptor).unwrap(),
            descriptor.reflection(&Budget::standard()).unwrap()
        );
        assert!(!native.portable_is_materialized());
        assert_eq!(
            native.portable_words().unwrap(),
            portable.stage.spirv.as_ref()
        );
        assert!(native.portable_is_materialized());
    }
}

#[test]
fn native_portable_materialization_is_shared_across_threads() {
    let bc = fixture(false);
    let f = Library::from_air(&bc)
        .unwrap()
        .function("copy_vector")
        .unwrap();
    let dev = device(true, false);
    let fd = FunctionDescriptor::new(f.clone());
    let native = Arc::new(
        dev.compile_native_function(&fd, &LoweringInputs::new(), &Budget::standard())
            .unwrap()
            .into_pipeline(&dev, &kernel(f))
            .unwrap(),
    );
    assert!(!native.portable_is_materialized());
    std::thread::scope(|scope| {
        let jobs: Vec<_> = (0..16)
            .map(|_| {
                let native = native.clone();
                scope.spawn(move || native.portable_words().unwrap().as_ptr() as usize)
            })
            .collect();
        let pointers: Vec<_> = jobs.into_iter().map(|j| j.join().unwrap()).collect();
        assert!(pointers.iter().all(|p| *p == pointers[0]));
    });
}
