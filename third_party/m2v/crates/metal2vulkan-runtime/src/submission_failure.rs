//! A batch's failure the driver reports outside Vulkan, asked for once the batch's value is
//! reached (a runtime extension, [`Device::set_submission_failure_hook`]).
//!
//! **Why.** Metal fails the command buffer a GPU fault strikes, and those committed while it ran,
//! and runs the queue on. Vulkan's only answer for failed work is a lost device, after which every
//! submission fails. A driver that recovers its queue from a fault (ios-on-switch-lmao's NVK, over
//! a kernel that kills only the faulting channel) signals the failed work's timeline values as
//! usual and keeps their failure: the completion thread asks for it, for each finished batch, and
//! the batch's command buffer finishes with it (`CommandBufferShared::complete`), as one whose
//! failure MoltenVK relayed does (`device/relay.rs`).

use super::device::DeviceShared;
use super::Device;
use ash::vk::Handle;

/// Asked on the completion thread once `semaphore` (a `VkSemaphore` handle), a `VkQueue`'s
/// timeline of the runtime's, reached `value`, the value one of its batches signals: 0 when the
/// batch's work ran to its end, else the `MTLCommandBufferError` code it failed with.
pub type SubmissionFailureHook = unsafe extern "C" fn(semaphore: u64, value: u64) -> u32;

impl Device {
    /// Ask `hook` for each finished batch's failure ([`SubmissionFailureHook`]); set once, and
    /// false if it was set already.
    ///
    /// # Safety
    ///
    /// `hook` must be callable on the device's completion thread for the device's life, and must
    /// not call back into the runtime.
    pub unsafe fn set_submission_failure_hook(&self, hook: SubmissionFailureHook) -> bool {
        self.shared().submission_failure.set(hook).is_ok()
    }
}

/// Whether the consumer set a hook, so the completion thread asks only then.
pub(crate) fn asked(shared: &DeviceShared) -> bool {
    shared.submission_failure.get().is_some()
}

/// The `MTLCommandBufferError` code the consumer's hook gives the batch of `VkQueue` `queue`
/// that signals `value`, reached; `None` without a hook or a failure. Inside a section of the
/// exit guard.
pub(crate) fn failure(shared: &DeviceShared, queue: usize, value: u64) -> Option<u64> {
    let hook = shared.submission_failure.get()?;
    // SAFETY: the hook's contract (`Device::set_submission_failure_hook`); the timeline is the
    // device's, live while the device is.
    let code = unsafe { hook(shared.queues[queue].timeline.as_raw(), value) };
    (code != 0).then_some(u64::from(code))
}
