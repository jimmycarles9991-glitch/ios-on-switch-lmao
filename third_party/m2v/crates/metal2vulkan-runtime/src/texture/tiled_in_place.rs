//! A texture over optimal-tiled texels another driver of the process lays out in host memory it
//! shares with the device: the GM20B's block-linear IOSurfaces and the raw GM20B path's textures
//! and mip chains (`drivers/TegraGM20B/TegraGM20BLayout.h`), which are the device's own
//! optimal-tiled layout of the same descriptor (NIL's). The device's optimal image of the
//! descriptor bound at the texels' offset in the buffer's memory reads and writes those texels: a
//! view, never a copy, of any type but a texture buffer, any levels, layers and samples.
//!
//! The image is made for host allocations, as Vulkan binds no other image to imported host
//! memory; the device must take optimal tiling for such images (NVK on a Tegra SoC does). Its
//! first move acquires it from `VK_QUEUE_FAMILY_EXTERNAL` in `GENERAL`, so the texels written
//! outside the device stay. Its bytes are the buffer's, tracked under the buffer's key, so a
//! command that writes through the one orders before a read through the other. CPU copies of it
//! take the device's own route for an optimal image (staging copies).

use super::super::address::ObjectsToken;
use super::super::buffer::{Buffer, CallerMemory};
use super::super::error::{Error, RuntimeGap};
use super::super::image_plan::{check_image_limits, plan_image};
use super::super::resource::Identity;
use super::super::texture_descriptor::{texture_usage, TextureDescriptor};
use super::super::values::StorageMode;
use super::super::view_plan::ViewRecord;
use super::super::Device;
use super::first_move::FirstMove;
use super::{image_error, IdUse, Image, Owner, Raw, Storage, Texture};
use crate::release;
use ash::vk;
use metal2vulkan::metal::format::FormatEmulation;
use metal2vulkan::metal::TextureType;
use metal2vulkan::vk::ImageAspectFlags;
use std::sync::atomic::AtomicBool;
use std::sync::Mutex;

impl Buffer {
    /// The device's optimal image of `descriptor` bound `offset` bytes into the buffer's memory,
    /// holding the bytes there as the device's layout of that image reads them. The buffer is a
    /// no-copy buffer over host memory. Refused (`NotImplemented(Textures)`) for a texture
    /// buffer, a format whose image is not exactly its bytes (another texel size, a depth or
    /// stencil aspect), a buffer of other memory, or an image whose alignment, size or memory
    /// type does not fit the buffer at `offset`.
    pub fn new_tiled_texture_in_place(
        &self,
        descriptor: &TextureDescriptor,
        offset: u64,
    ) -> Result<Texture, Error> {
        self.device().tiled_texture_in_place(self, descriptor, offset)
    }
}

impl Device {
    fn tiled_texture_in_place(
        &self,
        buffer: &Buffer,
        descriptor: &TextureDescriptor,
        offset: u64,
    ) -> Result<Texture, Error> {
        const CALL: &str = "new_tiled_texture_in_place";
        let refused = Error::NotImplemented(RuntimeGap::Textures);
        let descriptor = &self.take_texture_options(CALL, descriptor);
        self.check_texture_placement(
            CALL,
            descriptor.placement,
            descriptor.usage,
            descriptor.texture_type,
        )?;
        let options = descriptor.resource_options;
        let hazard = options.hazard_tracking_mode()?;
        let layout = descriptor
            .check(self.properties().texture_rules())
            .map_err(|refused| refused.error(CALL))?;
        if layout.texture_type == TextureType::TextureBuffer
            || options.storage_mode()? == StorageMode::Memoryless
            || buffer.caller_memory() != Some(CallerMemory::HostPages)
        {
            return Err(refused);
        }
        if self.is_lost() {
            return Err(Error::DeviceLost);
        }
        let facts = self
            .shared()
            .textures
            .facts(self.compile_device().profile());
        let usage = texture_usage(descriptor.usage, descriptor.swizzle);
        let plan = plan_image(&layout, usage, false, &facts)
            .map_err(|refusal| image_error(CALL, refusal))?;
        let exact = matches!(
            plan.mapping.emulation,
            FormatEmulation::Direct | FormatEmulation::SwizzledView
        );
        if !exact || plan.mapping.aspects != ImageAspectFlags::COLOR {
            return Err(refused);
        }
        let shared = self.shared();
        let raw_device = shared.vk();
        let view_formats = plan.ash_view_formats();
        let mut list = vk::ImageFormatListCreateInfo::default();
        let mut external = vk::ExternalMemoryImageCreateInfo::default()
            .handle_types(vk::ExternalMemoryHandleTypeFlags::HOST_ALLOCATION_EXT);
        let info = plan
            .create_info(shared.sharing(), &view_formats, &mut list)
            .push_next(&mut external);
        let Ok(limits) = self.host_image_limits(&info, &view_formats)? else {
            return Err(refused);
        };
        check_image_limits(&plan, layout.texture_type, &limits).map_err(Error::Unsupported)?;
        // SAFETY: the device is live and `info` (with its chain) valid and within the device's
        // limits for an image of host allocations.
        let raw = unsafe { crate::creation::image(raw_device, &info) }?;
        let mut dedicated = vk::MemoryDedicatedRequirements::default();
        let requirements = {
            let mut requirements = vk::MemoryRequirements2::default().push_next(&mut dedicated);
            let query = vk::ImageMemoryRequirementsInfo2::default().image(raw);
            // SAFETY: `raw` is a live image of this device; the query is core in Vulkan 1.1.
            unsafe { raw_device.get_image_memory_requirements2(&query, &mut requirements) };
            requirements.memory_requirements
        };
        let (memory, buffer_offset, type_index) = buffer.memory();
        let at = buffer_offset + offset;
        let fits = dedicated.requires_dedicated_allocation == vk::FALSE
            && at % requirements.alignment.max(1) == 0
            && offset + requirements.size <= buffer.padded_length()
            && requirements.memory_type_bits & (1 << type_index) != 0;
        // SAFETY: the image is live and unbound, made for host allocations; the memory is the
        // buffer's, which the image holds, in a type the image takes, at an offset checked
        // against the image's alignment and size.
        if !fits || unsafe { raw_device.bind_image_memory(raw, memory, at) }.is_err() {
            // SAFETY: the image is live, and bound to nothing a command uses.
            unsafe { release::object(raw_device, raw) };
            return Err(refused);
        }
        let format = plan.format;
        let image = Image {
            device: self.clone(),
            raw: Raw::new(raw),
            owner: Owner::Buffer {
                memory: Mutex::new(buffer.memory_hold()),
            },
            plan,
            options,
            allow_gpu_optimized_contents: false,
            identity: Identity {
                tracked: Identity::new(hazard, false).tracked,
                ..buffer.identity()
            },
            views: Mutex::new(Default::default()),
            twin: None,
            decoded: None,
            form: Default::default(),
            kept_outside: AtomicBool::new(false),
            detached: Mutex::new(None),
            first_move: Mutex::new(Some(
                FirstMove::new(raw, format, vk::ImageLayout::UNDEFINED, false)
                    .acquired_external(self.foreign_acquirer()),
            )),
            host_copies: false,
            linear: false,
            staging_copies: false,
            objects: ObjectsToken::default(),
        };
        let record = ViewRecord::of_descriptor(&layout, descriptor);
        Ok(Texture::made(
            self,
            Storage::Image(image.shared()),
            record,
            None,
            IdUse::Program(descriptor.placement),
        ))
    }
}
