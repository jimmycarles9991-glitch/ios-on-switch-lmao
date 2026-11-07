//! The caller's host memory each of the runtime's `vkQueueSubmit`s uses, told to the consumer
//! just before the submission (a runtime extension, [`Device::set_submission_memory_hook`]), for
//! a driver that orders other processes' GPU work on that memory per submission.
//!
//! - **What counts.** The memory of every buffer over the caller's host pages (a no-copy buffer)
//!   a batch's encoders use, as its owner in `used_resources.rs` names it: a texture over such a
//!   buffer is the buffer's memory. A tracked use reads or writes as its encoder's summary says
//!   (`tracker.rs`); a key the encoder named outside the summary (an untracked resource, a
//!   binding no command reached) counts as both, and a resident one as its usage says.
//! - **When.** On the submitting thread, right before the `vkQueueSubmit` of a batch, under the
//!   queue's lock, and with an empty list before the relay's submission of no work. The device's
//!   own synchronous submissions (`device/submit.rs`) tell nothing: a driver treats a submission
//!   it was told nothing of as using every import.

use super::buffer::{Buffer, CallerMemory};
use super::device::DeviceShared;
use super::encoder::Recorded;
use super::resource::ResourceKey;
use super::texture::TextureMemory;
use super::used_resources::EncoderRecord;
use super::Device;
use ash::vk;
use std::collections::HashSet;

/// Told on the submitting thread just before each of the runtime's `vkQueueSubmit`s on `queue`:
/// `count` memories over the caller's host pages its command buffers use, each once, with its
/// access, 1 read, 2 write, 3 both.
pub type SubmissionMemoryHook =
    unsafe extern "C" fn(queue: vk::Queue, memories: *const vk::DeviceMemory, access: *const u8, count: u32);

pub(crate) const READ: u8 = 1;
pub(crate) const WRITE: u8 = 2;

/// The memories one submission uses, each once with the union of its accesses.
#[derive(Default)]
pub(crate) struct HostUses {
    memories: Vec<vk::DeviceMemory>,
    access: Vec<u8>,
}

impl HostUses {
    pub(crate) fn add(&mut self, memory: vk::DeviceMemory, access: u8) {
        match self.memories.iter().position(|&known| known == memory) {
            Some(at) => self.access[at] |= access,
            None => {
                self.memories.push(memory);
                self.access.push(access);
            }
        }
    }

    /// Tell the consumer's hook, if it set one, of the next submission on `queue`.
    pub(crate) fn tell(&self, shared: &DeviceShared, queue: vk::Queue) {
        if let Some(hook) = shared.submission_memory.get() {
            // SAFETY: the hook's contract (`Device::set_submission_memory_hook`); both slices
            // live through the call and hold `count` entries.
            unsafe {
                hook(
                    queue,
                    self.memories.as_ptr(),
                    self.access.as_ptr(),
                    self.memories.len() as u32,
                )
            };
        }
    }
}

/// The access bits of a use that reads (`read`) or writes (`write`).
pub(crate) fn access(read: bool, write: bool) -> u8 {
    (if read { READ } else { 0 }) | (if write { WRITE } else { 0 })
}

/// Whether a consumer asked to be told, so a submission gathers its uses only then.
pub(crate) fn wanted(shared: &DeviceShared) -> bool {
    shared.submission_memory.get().is_some()
}

/// The host memory the batch of `recorded` encoders uses, `ended` being their command buffer's
/// records of its ended encoders.
pub(crate) fn batch(
    shared: &DeviceShared,
    recorded: &[Recorded],
    ended: &[EncoderRecord],
) -> HostUses {
    let mut uses: Vec<(ResourceKey, u8)> = Vec::new();
    let mut host = HostUses::default();
    for encoder in recorded {
        let summarised: HashSet<ResourceKey> = encoder.summary.keys().collect();
        uses.extend(
            (encoder.summary.accesses()).map(|(key, read, write)| (key, access(read, write))),
        );
        let Some(record) = ended.get(encoder.encoder) else {
            continue;
        };
        let unsummarised = record.keys.iter().filter(|key| !summarised.contains(key));
        uses.extend(unsummarised.map(|&key| (key, READ | WRITE)));
        if let Some(residency) = &record.residency {
            for (buffer, written) in residency.buffers_written() {
                add_buffer(&mut host, buffer, written);
            }
            for (texture, written) in residency.textures_written() {
                if let TextureMemory::Buffer(buffer) = texture.memory() {
                    add_buffer(&mut host, buffer, written);
                }
            }
        }
    }
    for (buffer, access) in shared.owners.host_buffers(uses) {
        host.add(buffer.memory().0, access);
    }
    host
}

fn add_buffer(host: &mut HostUses, buffer: &Buffer, written: bool) {
    if buffer.heap().is_none() && buffer.caller_memory() == Some(CallerMemory::HostPages) {
        host.add(buffer.memory().0, access(true, written));
    }
}

impl Device {
    /// Tell `hook` before each of this device's command buffer submissions which memory over the
    /// caller's host pages it uses ([`SubmissionMemoryHook`]); set once, and false if it was set
    /// already.
    ///
    /// # Safety
    ///
    /// `hook` must be callable on any thread that submits, under the runtime's queue lock, for
    /// the device's life, and must not call back into the runtime.
    pub unsafe fn set_submission_memory_hook(&self, hook: SubmissionMemoryHook) -> bool {
        self.shared().submission_memory.set(hook).is_ok()
    }
}
