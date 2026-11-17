//! A handle in an array of arrays of an argument buffer (`array<array<sampler, 2>, 3>`,
//! `air.struct_type_info`'s one member of `array_len` outer elements of `size` bytes, its role's
//! location count the handles in each): a load of any element names the member, as a load of any
//! element of a one-dimensional `array<sampler, N>` does. Metal's encoder lays the elements out
//! flat (`bind::argument_buffer`), so a shader reads them at every handle's offset.

use crate::air::abi::{
    AbiInt, ArgTypeInfo, IndirectArg, Member, MemberKind, ParamRole, Slot, StructInfo, StructRef,
    UserKey,
};
use crate::air::error::DiagName;
use crate::budget::{Budget, Meter};
use crate::memory::abi_index::AbiIndex;
use crate::memory::provenance::member_element_size;

/// Samplers at offset 0: `outer` elements (0: no member array) of `count` handles each.
fn samplers(outer: u32, count: u32) -> Member {
    Member {
        offset: 0,
        size: 8 * count,
        array_len: outer,
        layout: None,
        kind: MemberKind::Indirect(IndirectArg {
            ordinal: 0,
            role: ParamRole::Sampler {
                location: Slot {
                    index: AbiInt::Const(0),
                    count: AbiInt::Const(count),
                },
            },
            ty: ArgTypeInfo {
                size: None,
                align: None,
                name: None,
            },
            name: None,
        }),
        render_target: None,
        raster_order_group: None,
        type_name: DiagName::new("sampler"),
        name: DiagName::new("s"),
        key: UserKey(Box::new([])),
    }
}

#[test]
fn every_handle_of_an_array_of_arrays_is_an_element() {
    for (outer, count) in [(0, 1), (0, 4), (4, 1), (3, 2), (2, 4)] {
        assert_eq!(
            member_element_size(&samplers(outer, count)),
            8,
            "{outer} x {count}"
        );
    }
    let structs = vec![StructInfo {
        members: vec![samplers(3, 2)],
    }];
    let mut m = Meter::new(&Budget::standard());
    let abi = AbiIndex::layouts(&structs, &mut m).expect("an index");
    for at in (0..48).step_by(8) {
        assert!(
            abi.member_place_at(&structs, StructRef(0), at).is_some(),
            "the handle at {at}"
        );
    }
    assert!(
        abi.member_place_at(&structs, StructRef(0), 4).is_none(),
        "inside a handle"
    );
}
