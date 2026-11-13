// SPDX-License-Identifier: MIT
//! What a library and a function handle hold (NativeMetalObjects.h's NmbLibrary, NmbFunction): a runtime device's library
//! and function, or, made without a device (NativeMetalCompile.rs), the translator's own library and the function with the
//! constants it was made with. A function of either answers `MTLFunction`'s questions as the runtime's library.rs answers
//! them, from the translator's function, so a state compiled from either keys, reflects and translates alike.

use crate::objects::e;
use metal2vulkan::Budget;
use metal2vulkan::library::{FunctionConstantValues, LibraryType};
use metal2vulkan::metal::FunctionType;
use metal2vulkan::pipeline::{FunctionDescriptor as StageFunction, LinkedFunction};
use metal2vulkan::reflection::{Attribute, FunctionConstant, FunctionProperties};
use metal2vulkan_runtime::{Function, Library};
use std::collections::BTreeMap;

pub(crate) enum LibraryOf {
    Runtime(Library),
    Compile(metal2vulkan::library::Library),
}

/// A function made without a device, as the runtime's `Function` holds it but for the device
pub(crate) struct CompileFunction {
    function: metal2vulkan::library::Function,
    constants: FunctionConstantValues,
    specialized: bool,
}

pub(crate) enum FunctionOf {
    Runtime(Function),
    Compile(CompileFunction),
}

impl LibraryOf {
    pub(crate) fn function_names(&self) -> Vec<String> {
        match self {
            LibraryOf::Runtime(library) => library.function_names(),
            LibraryOf::Compile(library) => library.functions().map(|info| info.name.to_string()).collect(),
        }
    }

    /// `newFunctionWithName:`, refused under `what`
    pub(crate) fn function(&self, name: &str, what: &'static str) -> Result<FunctionOf, String> {
        match self {
            LibraryOf::Runtime(library) => Ok(FunctionOf::Runtime(
                library.new_function_with_name(name).map_err(e(what))?,
            )),
            LibraryOf::Compile(library) => compiled(library, name, FunctionConstantValues::defaults(), false, what),
        }
    }

    /// `newFunctionWithName:constantValues:error:`, refused under `what`
    pub(crate) fn specialized(
        &self,
        name: &str,
        constants: &FunctionConstantValues,
        what: &'static str,
    ) -> Result<FunctionOf, String> {
        match self {
            LibraryOf::Runtime(library) => Ok(FunctionOf::Runtime(
                library
                    .new_function_with_name_constant_values(name, constants)
                    .map_err(e(what))?,
            )),
            LibraryOf::Compile(library) => compiled(library, name, constants.clone(), true, what),
        }
    }
}

/// The runtime's refusal of a dynamic library's function (library.rs Library::function), else the translator's function
fn compiled(
    library: &metal2vulkan::library::Library,
    name: &str,
    constants: FunctionConstantValues,
    specialized: bool,
    what: &'static str,
) -> Result<FunctionOf, String> {
    if library.library_type() == LibraryType::Dynamic {
        return Err(e(what)("a dynamic library's function is no stage"));
    }
    Ok(FunctionOf::Compile(CompileFunction {
        function: library.function(name).map_err(e(what))?,
        constants,
        specialized,
    }))
}

impl CompileFunction {
    fn properties(&self, what: &'static str) -> Result<FunctionProperties, String> {
        let budget = Budget::standard();
        if self.specialized {
            self.function
                .specialized_properties(&self.constants, &budget)
                .map_err(e(what))
        } else {
            self.function.properties(&budget).map_err(e(what))
        }
    }
}

impl FunctionOf {
    /// The runtime's function, for what only a device makes (pipeline states, argument encoders)
    pub(crate) fn runtime(&self) -> Result<&Function, String> {
        match self {
            FunctionOf::Runtime(function) => Ok(function),
            FunctionOf::Compile(_) => Err("a function made without a device".into()),
        }
    }

    pub(crate) fn function_descriptor(&self) -> StageFunction {
        match self {
            FunctionOf::Runtime(function) => function.function_descriptor(),
            FunctionOf::Compile(f) => {
                let mut descriptor = StageFunction::new(f.function.clone());
                descriptor.constants = f.constants.clone();
                descriptor
            }
        }
    }

    pub(crate) fn linked_function(&self) -> LinkedFunction {
        match self {
            FunctionOf::Runtime(function) => function.linked_function(),
            FunctionOf::Compile(f) => LinkedFunction::new(f.function.clone())
                .with_constants(f.constants.clone())
                .with_precompiled(false),
        }
    }

    pub(crate) fn function_constants_dictionary(
        &self,
        what: &'static str,
    ) -> Result<BTreeMap<String, FunctionConstant>, String> {
        match self {
            FunctionOf::Runtime(function) => function.function_constants_dictionary().map_err(e(what)),
            FunctionOf::Compile(f) if f.specialized => Ok(BTreeMap::new()),
            FunctionOf::Compile(f) => f.function.function_constants_dictionary().map_err(e(what)),
        }
    }

    /// `stageInputAttributes` (`stage_input`) or `vertexAttributes`, refused under `what`
    pub(crate) fn attributes(&self, stage_input: bool, what: &'static str) -> Result<Option<Vec<Attribute>>, String> {
        match self {
            FunctionOf::Runtime(function) if stage_input => function.stage_input_attributes().map_err(e(what)),
            FunctionOf::Runtime(function) => function.vertex_attributes().map_err(e(what)),
            FunctionOf::Compile(f) => {
                let properties = f.properties(what)?;
                let attributes = if stage_input {
                    properties.stage_input_attributes()
                } else {
                    properties.vertex_attributes()
                };
                Ok(attributes.map(<[Attribute]>::to_vec))
            }
        }
    }

    pub(crate) fn function_type(&self, what: &'static str) -> Result<FunctionType, String> {
        match self {
            FunctionOf::Runtime(function) => function.function_type().map_err(e(what)),
            FunctionOf::Compile(f) => Ok(f.properties(what)?.function_type()),
        }
    }
}
