// SPDX-License-Identifier: MIT
use metal2vulkan::reflection::{
    ArrayType, Binding, ComputePipelineReflection, PointerType, StructMember, StructType, TextureReferenceType,
};
use std::ffi::{CString, c_char};

const ABSENT: u32 = u32::MAX;

#[repr(C)]
pub struct NmbReflectionNode {
    pub kind: u32,
    pub flags: u32,
    pub name: *const c_char,
    pub values: [u64; 8],
    pub children: [u32; 4],
    pub first: u32,
    pub count: u32,
}

impl NmbReflectionNode {
    fn new(kind: u32) -> Self {
        Self {
            kind,
            flags: 0,
            name: std::ptr::null(),
            values: [0; 8],
            children: [ABSENT; 4],
            first: 0,
            count: 0,
        }
    }
}

pub(crate) struct Reflection {
    pub nodes: Vec<NmbReflectionNode>,
    pub edges: Vec<u32>,
    names: Vec<CString>,
}

// Nodes point only into this immutable snapshot's separately allocated CString storage.
unsafe impl Send for Reflection {}
unsafe impl Sync for Reflection {}

impl Reflection {
    pub(crate) fn from_parts(
        mut nodes: Vec<NmbReflectionNode>,
        edges: Vec<u32>,
        names: Vec<Option<CString>>,
    ) -> Option<Self> {
        if nodes.is_empty() || nodes.len() != names.len() || nodes[0].kind != 0 {
            return None;
        }
        let mut depth = vec![0usize; nodes.len()];
        for (i, node) in nodes.iter().enumerate() {
            if node.kind > 6
                || (i != 0 && node.kind == 0)
                || node.first as usize > edges.len()
                || node.count as usize > edges.len() - node.first as usize
            {
                return None;
            }
            let children = &edges[node.first as usize..node.first as usize + node.count as usize];
            if node.kind != 0 && node.kind != 2 && !children.is_empty() {
                return None;
            }
            for &child in children {
                if child as usize >= nodes.len()
                    || (i != 0 && child as usize >= i)
                    || nodes[child as usize].kind != (if i == 0 { 1 } else { 3 })
                {
                    return None;
                }
                if i != 0 {
                    depth[i] = depth[i].max(depth[child as usize] + 1);
                }
            }
            for (slot, &child) in node.children.iter().enumerate() {
                if child == ABSENT {
                    continue;
                }
                if child as usize >= i
                    || !matches!(node.kind, 1 | 3 | 4 | 5)
                    || nodes[child as usize].kind != [2, 4, 6, 5][slot]
                {
                    return None;
                }
                depth[i] = depth[i].max(depth[child as usize] + 1);
            }
            if depth[i] > 64 {
                return None;
            }
        }
        let mut owned = Vec::new();
        for (node, name) in nodes.iter_mut().zip(names) {
            if let Some(name) = name {
                owned.push(name);
                node.name = owned.last()?.as_ptr();
            }
        }
        Some(Self {
            nodes,
            edges,
            names: owned,
        })
    }

    pub fn of(reflection: &ComputePipelineReflection) -> Self {
        let mut out = Self {
            nodes: vec![NmbReflectionNode::new(0)],
            edges: Vec::new(),
            names: Vec::new(),
        };
        let bindings: Vec<_> = reflection.bindings().iter().map(|b| out.binding(b)).collect();
        out.nodes[0].first = out.edges.len() as u32;
        out.nodes[0].count = bindings.len() as u32;
        out.edges.extend(bindings);
        out
    }

    fn named(&mut self, node: &mut NmbReflectionNode, name: &str) {
        self.names.push(CString::new(name).unwrap_or_default());
        node.name = self.names.last().unwrap().as_ptr();
    }

    fn push(&mut self, node: NmbReflectionNode) -> u32 {
        let index = self.nodes.len() as u32;
        self.nodes.push(node);
        index
    }

    fn binding(&mut self, binding: &Binding) -> u32 {
        let mut node = NmbReflectionNode::new(1);
        self.named(&mut node, binding.name());
        node.flags = u32::from(binding.is_used()) | (u32::from(binding.is_argument()) << 1);
        node.values[0] = binding.binding_type().as_raw().into();
        node.values[1] = binding.access().as_raw().into();
        node.values[2] = binding.index().into();
        if let Some(buffer) = binding.buffer() {
            node.values[5] = buffer.buffer_alignment();
            node.values[6] = buffer.buffer_data_size();
            node.values[7] = buffer.buffer_data_type().as_raw().into();
            node.children[0] = buffer.buffer_struct_type().map_or(ABSENT, |s| self.structure(s));
            node.children[3] = buffer.buffer_pointer_type().map_or(ABSENT, |p| self.pointer(p));
        } else if let Some(memory) = binding.threadgroup() {
            node.values[5] = memory.threadgroup_memory_alignment();
            node.values[6] = memory.threadgroup_memory_data_size();
        } else if let Some(texture) = binding.texture() {
            node.values[5] = texture.texture_type().as_raw().into();
            node.values[6] = texture.texture_data_type().as_raw().into();
            node.values[7] = texture.array_length().into();
            node.flags |= u32::from(texture.is_depth_texture()) << 2;
        }
        self.push(node)
    }

    fn structure(&mut self, structure: &StructType) -> u32 {
        let mut node = NmbReflectionNode::new(2);
        let members: Vec<_> = structure.members().iter().map(|m| self.member(m)).collect();
        node.first = self.edges.len() as u32;
        node.count = members.len() as u32;
        self.edges.extend(members);
        self.push(node)
    }

    fn member(&mut self, member: &StructMember) -> u32 {
        let mut node = NmbReflectionNode::new(3);
        self.named(&mut node, member.name());
        node.values[0] = member.data_type().as_raw().into();
        node.values[1] = member.offset();
        node.values[2] = member.argument_index().into();
        node.children[0] = member.struct_type().map_or(ABSENT, |s| self.structure(s));
        node.children[1] = member.array_type().map_or(ABSENT, |a| self.array(a));
        node.children[2] = member.texture_reference_type().map_or(ABSENT, |t| self.texture(t));
        node.children[3] = member.pointer_type().map_or(ABSENT, |p| self.pointer(p));
        self.push(node)
    }

    fn array(&mut self, array: &ArrayType) -> u32 {
        let mut node = NmbReflectionNode::new(4);
        node.values[0] = array.element_type().as_raw().into();
        node.values[1] = array.array_length().into();
        node.values[2] = array.stride();
        node.values[3] = array.argument_index_stride().into();
        node.children[0] = array.element_struct_type().map_or(ABSENT, |s| self.structure(s));
        node.children[1] = array.element_array_type().map_or(ABSENT, |a| self.array(a));
        node.children[2] = array
            .element_texture_reference_type()
            .map_or(ABSENT, |t| self.texture(t));
        node.children[3] = array.element_pointer_type().map_or(ABSENT, |p| self.pointer(p));
        self.push(node)
    }

    fn pointer(&mut self, pointer: &PointerType) -> u32 {
        let mut node = NmbReflectionNode::new(5);
        node.values[0] = pointer.element_type().as_raw().into();
        node.values[1] = pointer.access().as_raw().into();
        node.values[2] = pointer.alignment();
        node.values[3] = pointer.data_size();
        node.flags = u32::from(pointer.element_is_argument_buffer());
        node.children[0] = pointer.element_struct_type().map_or(ABSENT, |s| self.structure(s));
        node.children[1] = pointer.element_array_type().map_or(ABSENT, |a| self.array(a));
        self.push(node)
    }

    fn texture(&mut self, texture: &TextureReferenceType) -> u32 {
        let mut node = NmbReflectionNode::new(6);
        node.values[0] = texture.texture_data_type().as_raw().into();
        node.values[1] = texture.texture_type().as_raw().into();
        node.values[2] = texture.access().as_raw().into();
        node.flags = u32::from(texture.is_depth_texture());
        self.push(node)
    }
}
