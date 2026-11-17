use super::*;
use crate::pipeline::descriptor::LinkedList;

#[test]
fn grouped_indirect_calls_keep_exact_outputs_and_distinct_identity() {
    let bytes =
        fixture("grouped_pointer").expect("llvm-as is required for the grouped pointer fixture");
    let library = Library::from_air(&bytes).expect("library");
    let device = device("moltenvk-m3");
    let none = FunctionConstantValues::defaults();
    let sources = ["grouped_add", "grouped_mul"].map(|name| {
        let mut source = LinkedSource::new(name, &bytes, &none);
        source.key.list = LinkedList::Group("choice");
        source
    });
    let p = profile("moltenvk-m3", DevicePolicy::default()).expect("profile");
    let artifact = compile_bitcode_linked(
        &bytes,
        EntryRef::Name("grouped_pointer"),
        (&none, &sources),
        &p,
        &LowerInputs::new(),
        &mut meter(),
        SPIRV_WORDS,
    )
    .expect("grouped indirect call");
    assert!(artifact.interface.numbered_samplers.is_empty());
    link_and_validate(&artifact, &p).expect("grouped indirect call validates");
    let (_, module) = typed_module_linked(&bytes, "grouped_pointer", &p, &sources).expect("module");
    const N: u32 = 32;
    let result = run(
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
    .expect("dispatch");
    let expected = (0..N)
        .flat_map(|tid| (if tid & 1 != 0 { tid + 5 } else { tid * 3 }).to_le_bytes())
        .collect::<Vec<_>>();
    assert_eq!(result.buffers[0], expected);

    let linked = |name: &str, reverse: bool| {
        let mut members = ["grouped_add", "grouped_mul"]
            .map(|name| LinkedFunction::new(library.function(name).expect("function")))
            .to_vec();
        if reverse {
            members.reverse();
        }
        let mut functions = LinkedFunctions::new();
        functions.groups.insert(name.into(), members);
        functions
    };
    let descriptor = |linked| {
        FunctionDescriptor::new(library.function("grouped_pointer").expect("kernel"))
            .with_linked(linked)
    };
    let key = |linked| device.function_compile_key(&descriptor(linked), &LowerInputs::new());
    assert_ne!(key(linked("choice", false)), key(linked("other", false)));
    assert_ne!(
        key(linked("choice", false)),
        key(linked("choice\0other", false))
    );
    assert_ne!(key(linked("choice", false)), key(linked("choice", true)));
    for functions in [
        LinkedFunctions::new(),
        linked("choice", false),
        linked("choice", true),
    ] {
        let d = ComputePipelineDescriptor::new(StageSource::Function(descriptor(functions)));
        device
            .compile_compute_pipeline(&d, &Budget::standard())
            .expect("native grouped descriptor");
    }
}
