//! A render variant's `VkPipeline`s linked from graphics pipeline libraries (the coordinator's
//! fast-link stream: "compile each stage once per function into a library, and make a state's first
//! pipeline by a fast link without link-time optimisation, in milliseconds").
//!
//! Where the device enables `VK_EXT_graphics_pipeline_library` and links its libraries fast
//! (`graphicsPipelineLibraryFastLinking`; `device/create.rs`), a render variant's `VkPipeline` of
//! a graphics key is linked, without link-time optimization, from four libraries, one per part
//! of the pipeline's state (`graphics.rs`, [`GraphicsContent`]): its vertex input, its
//! pre-rasterization shaders, its fragment shader and its fragment output. Each part's library is
//! the device's, found by the exact content of its part ([`Part`], a digest of everything its
//! create info holds: the layout's description, each stage's words, the constants its stages
//! declare with their values, the part's state, the pass and the dynamic states), so the keys and
//! states that share a part share its library: a fragment function's library is compiled once
//! per distinct fragment specialization, layout, fragment state and pass, and each key links in
//! milliseconds. The pass is in every part's name because the libraries a pipeline links must
//! be made against compatible render passes (`VUID-VkGraphicsPipelineCreateInfo-pLibraries-06628`),
//! and a pass's compatibility includes its attachments' formats.
//!
//! - **Same pipeline.** The libraries are made from the very create infos the whole pipeline's
//!   making takes, split by part; linking them is the driver's, so a linked pipeline is the
//!   whole pipeline the same state describes. Nothing an answer depends on changes.
//! - **The device's, by content** (`content_keyed.rs`). A program's call (a draw, a state's
//!   creation) claims each part's library, inserting it where the device has none; a pool piece
//!   only looks up, and on a miss makes one of its own that no lookup finds. A claimed library is
//!   made by the first caller that needs it made ([`Library::made`]); a caller meanwhile waits for
//!   that making, and one after a failed making makes its own attempt, as a draw's own making does
//!   (`making.rs`).
//! - **Alive while linked.** A linked pipeline holds its four libraries ([`Linked`]), and a kept
//!   pipeline keeps them (`kept.rs`): the device's map destroys a library only once nothing holds
//!   it, at a program's settle or the device's teardown, after every pipeline it linked.
//! - **Cached across processes.** Each library of shaders (pre-rasterization, fragment shader) is
//!   made through the device's shader cache under a record named by its part
//!   (`shader_cache/driver.rs`), as a whole pipeline is; the vertex input and fragment output
//!   libraries hold no shader and keep no record.

use super::budget::Turn;
use super::content_keyed::{program_call, ContentKeyed, Gone, Owned};
use crate::device::alongside::alongside;
use crate::device::{lock, DeviceShared};
use crate::error::Error;
use crate::release;
use ash::vk;
use metal2vulkan::cache::Hash256;
use std::sync::{Arc, Condvar, Mutex, PoisonError};

/// The four parts of a graphics pipeline's state, in the order a linked pipeline takes them.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum Part {
    VertexInput,
    PreRasterization,
    FragmentShader,
    FragmentOutput,
}

impl Part {
    pub(crate) const ALL: [Part; 4] = [
        Part::VertexInput,
        Part::PreRasterization,
        Part::FragmentShader,
        Part::FragmentOutput,
    ];

    /// Its library's flag.
    pub(crate) fn flag(self) -> vk::GraphicsPipelineLibraryFlagsEXT {
        use vk::GraphicsPipelineLibraryFlagsEXT as F;
        match self {
            Part::VertexInput => F::VERTEX_INPUT_INTERFACE,
            Part::PreRasterization => F::PRE_RASTERIZATION_SHADERS,
            Part::FragmentShader => F::FRAGMENT_SHADER,
            Part::FragmentOutput => F::FRAGMENT_OUTPUT_INTERFACE,
        }
    }

    /// Whether its library compiles shaders: the pre-rasterization and fragment shader parts.
    /// Only those are made through the device's shader cache, each under a record of its own
    /// (`shader_cache/driver.rs`); the vertex input and fragment output libraries hold no shader,
    /// and are made without a pipeline cache.
    pub(crate) fn holds_shaders(self) -> bool {
        matches!(self, Part::PreRasterization | Part::FragmentShader)
    }

    /// The name its library's making record is begun with (`shader_cache/driver.rs`).
    pub(crate) fn kind(self) -> &'static str {
        match self {
            Part::VertexInput => "vertex input library",
            Part::PreRasterization => "pre-rasterization library",
            Part::FragmentShader => "fragment shader library",
            Part::FragmentOutput => "fragment output library",
        }
    }
}

/// One part's library: the device's, by its part's content, made once by the first caller that
/// needs it ([`Library::made`]).
pub(crate) struct Library {
    state: Mutex<Slot>,
    /// Signalled as a making ends.
    ended: Condvar,
}

/// Where a library's making is.
enum Slot {
    /// Not made: the next caller that needs it makes it.
    Waiting,
    /// A caller is making it.
    Making,
    Made(vk::Pipeline),
}

/// A caller's making of a library: where it ends without a library (an error, or a panic), the
/// library waits for the next caller again, and those waiting are woken.
pub(crate) struct Claim<'a>(&'a Library);

impl Claim<'_> {
    /// The library made by `make`, on whichever thread holds the claim.
    pub(crate) fn made(
        self,
        make: impl FnOnce() -> Result<vk::Pipeline, Error>,
    ) -> Result<vk::Pipeline, Error> {
        let raw = make()?;
        *lock(&self.0.state) = Slot::Made(raw);
        Ok(raw)
    }
}

impl Drop for Claim<'_> {
    fn drop(&mut self) {
        let mut state = lock(&self.0.state);
        if matches!(*state, Slot::Making) {
            *state = Slot::Waiting;
        }
        drop(state);
        self.0.ended.notify_all();
    }
}

impl Library {
    fn new() -> Library {
        Library {
            state: Mutex::new(Slot::Waiting),
            ended: Condvar::new(),
        }
    }

    /// Whether it is made.
    pub(crate) fn is_made(&self) -> bool {
        matches!(*lock(&self.state), Slot::Made(_))
    }

    /// The library, made now by `make` where no caller made it or is making it, else the one
    /// another caller made, waited for. A failed making leaves it to the next caller, as one of
    /// a key's draw does (`making.rs`).
    pub(crate) fn made(
        &self,
        make: impl FnOnce() -> Result<vk::Pipeline, Error>,
    ) -> Result<vk::Pipeline, Error> {
        let mut state = lock(&self.state);
        loop {
            match *state {
                Slot::Made(raw) => return Ok(raw),
                Slot::Making => {
                    state = self
                        .ended
                        .wait(state)
                        .unwrap_or_else(PoisonError::into_inner);
                }
                Slot::Waiting => break,
            }
        }
        *state = Slot::Making;
        drop(state);
        Claim(self).made(make)
    }

    /// Its making, claimed now where no caller made it or is making it: the claim's holder makes
    /// it ([`Claim::made`]), and other callers wait for that.
    pub(crate) fn claim(&self) -> Option<Claim<'_>> {
        let mut state = lock(&self.state);
        match *state {
            Slot::Waiting => {
                *state = Slot::Making;
                Some(Claim(self))
            }
            Slot::Making | Slot::Made(_) => None,
        }
    }
}

impl Owned<DeviceShared> for Library {
    fn destroy(&self, shared: &DeviceShared) {
        if let Slot::Made(raw) = *lock(&self.state) {
            // SAFETY: the map lets a library go only when nothing else holds it, so no pipeline
            // it linked is left (each holds it, and a command buffer holds the pipelines its
            // commands use until it completes) and none is being linked; or at the device's
            // teardown, when no variant, kept pipeline or command buffer is left.
            unsafe { release::object(shared.vk(), raw) };
        }
    }
}

/// The four libraries a linked pipeline was linked from, held while it lives.
#[derive(Clone)]
pub(crate) struct Linked(pub(crate) [Arc<Library>; 4]);

/// The device's libraries, by part and content (`DeviceShared::libraries`).
#[derive(Default)]
pub(crate) struct Libraries {
    parts: [Mutex<ContentKeyed<Hash256, Library>>; 4],
}

impl Libraries {
    /// The libraries of the four parts `names` names (each its part's digest, in
    /// [`Part::ALL`]'s order): the device's, inserted where it holds none in a program's call
    /// (`content_keyed::program_call`), else a caller's own on a miss. Each map sweeps.
    pub(crate) fn claim(&self, shared: &DeviceShared, names: &[Hash256; 4]) -> Linked {
        let program = program_call();
        let claimed = std::array::from_fn(|at| {
            let mut map = lock(&self.parts[at]);
            let (library, _) = match map.get_or_make(names[at], program, || {
                Ok::<Library, std::convert::Infallible>(Library::new())
            }) {
                Ok(found) => found,
                Err(never) => match never {},
            };
            let gone = map.sweep();
            drop(map);
            // Destroyed here, the map's lock released.
            gone.destroy(shared);
            library
        });
        Linked(claimed)
    }

    /// Let go of the libraries nothing holds any more: in a program's call, as the device's
    /// modules are (`objects.rs`).
    pub(crate) fn settle(&self, shared: &DeviceShared) {
        for part in &self.parts {
            let gone = lock(part).settle();
            gone.destroy(shared);
        }
    }

    /// Every library, at the device's teardown, after the kept pipelines it linked.
    pub(crate) fn drain(&self) -> Vec<Gone<Library>> {
        self.parts.iter().map(|part| lock(part).drain()).collect()
    }
}

impl Linked {
    /// Each part's library, in [`Part::ALL`]'s order, made by `make` where no caller made it or
    /// is making it, else waited for ([`Library::made`]). Where this thread makes the
    /// pre-rasterization library and a making turn is spare (`budget::spare_turn`), the fragment
    /// shader library, the other that compiles shaders, is made at once on a thread of the
    /// runtime's (`device/alongside.rs`): a creation's two compiles take the longer one's time,
    /// not their sum. The first failure in that order is the creation's, as when made one after
    /// another; a library made meanwhile stays made for the next caller.
    pub(crate) fn made(
        &self,
        make: impl Fn(Part) -> Result<vk::Pipeline, Error> + Sync,
    ) -> Result<[vk::Pipeline; 4], Error> {
        self.made_with(super::budget::spare_turn, make)
    }

    /// [`made`](Self::made), its spare turn from `spare`.
    pub(crate) fn made_with(
        &self,
        spare: impl FnOnce() -> Option<Turn>,
        make: impl Fn(Part) -> Result<vk::Pipeline, Error> + Sync,
    ) -> Result<[vk::Pipeline; 4], Error> {
        let made_here = |part: Part| self.library(part).made(|| make(part));
        let fragment = match self.library(Part::PreRasterization).is_made() {
            true => None,
            false => {
                spare().and_then(|turn| Some((turn, self.library(Part::FragmentShader).claim()?)))
            }
        };
        let mut raws = [vk::Pipeline::null(); 4];
        match fragment {
            None => {
                for part in Part::ALL {
                    raws[part as usize] = made_here(part)?;
                }
            }
            Some((turn, claim)) => {
                let (fragment, before) = alongside(
                    turn,
                    || claim.made(|| make(Part::FragmentShader)),
                    || {
                        Ok::<_, Error>([
                            made_here(Part::VertexInput)?,
                            made_here(Part::PreRasterization)?,
                        ])
                    },
                );
                let [vertex_input, pre_rasterization] = before?;
                raws[Part::VertexInput as usize] = vertex_input;
                raws[Part::PreRasterization as usize] = pre_rasterization;
                raws[Part::FragmentShader as usize] = fragment?;
                raws[Part::FragmentOutput as usize] = made_here(Part::FragmentOutput)?;
            }
        }
        Ok(raws)
    }

    fn library(&self, part: Part) -> &Library {
        &self.0[part as usize]
    }

    /// Whether the libraries of its shaders, its pre-rasterization and fragment shader parts,
    /// are made: a key whose shaders' libraries are made links on the caller's thread, where its
    /// vertex input and fragment output libraries, which hold no shader, are made in well under a
    /// millisecond if they are not made yet (EV PRDG-mk1-5080: 0.0 ms each on an RTX 5080, the
    /// link 0.0 ms, the pre-rasterization library 3 to 7 ms, the fragment shader library the
    /// whole pipeline's 20 to 38 s).
    pub(crate) fn shaders_made(&self) -> bool {
        Part::ALL
            .into_iter()
            .filter(|part| part.holds_shaders())
            .all(|part| self.0[part as usize].is_made())
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::error::internal;
    use crate::exit_guard;
    use crate::pipeline::budget::{ticket, waited_turn, Budget, Urgency};
    use ash::vk::Handle;
    use std::sync::mpsc;
    use std::thread::{Builder, ThreadId};
    use std::time::Duration;

    const WAIT: Duration = Duration::from_secs(10);

    fn raw(n: u64) -> vk::Pipeline {
        vk::Pipeline::from_raw(n)
    }

    fn linked() -> Linked {
        Linked(std::array::from_fn(|_| Arc::new(Library::new())))
    }

    fn part_raw(part: Part) -> vk::Pipeline {
        raw(part as u64 + 1)
    }

    /// Two makings that each wait, at most [`WAIT`], until both reached it: whether they did.
    #[derive(Default)]
    struct Meeting {
        arrived: Mutex<usize>,
        all: Condvar,
    }

    impl Meeting {
        fn meet(&self) -> bool {
            let mut arrived = lock(&self.arrived);
            *arrived += 1;
            self.all.notify_all();
            let (arrived, _) = self
                .all
                .wait_timeout_while(arrived, WAIT, |arrived| *arrived < 2)
                .unwrap();
            *arrived >= 2
        }
    }

    /// The parts made, each with the thread that made it.
    #[derive(Default)]
    struct Makings(Mutex<Vec<(Part, ThreadId)>>);

    impl Makings {
        fn made(&self, part: Part) -> Result<vk::Pipeline, Error> {
            lock(&self.0).push((part, std::thread::current().id()));
            Ok(part_raw(part))
        }

        fn parts(&self) -> Vec<Part> {
            lock(&self.0).iter().map(|(part, _)| *part).collect()
        }

        fn thread_of(&self, part: Part) -> ThreadId {
            lock(&self.0)
                .iter()
                .find(|(made, _)| *made == part)
                .unwrap()
                .1
        }
    }

    #[test]
    fn a_creation_makes_its_two_shader_libraries_at_once_with_a_spare_turn() {
        let budget = Arc::new(Budget::new(4, 3));
        let _mine = waited_turn(&budget, ticket());
        let linked = linked();
        let meeting = Meeting::default();
        let makings = Makings::default();
        let made = linked
            .made_with(
                || budget.spare(Urgency::Program),
                |part| {
                    if part.holds_shaders() {
                        assert!(meeting.meet(), "{part:?} made while the other waited");
                    }
                    makings.made(part)
                },
            )
            .unwrap();
        assert_eq!(made, Part::ALL.map(part_raw));
        assert!(Part::ALL
            .iter()
            .all(|part| linked.0[*part as usize].is_made()));
        let here = std::thread::current().id();
        assert_eq!(makings.thread_of(Part::PreRasterization), here);
        assert_ne!(makings.thread_of(Part::FragmentShader), here);
        assert_eq!(budget.census().0, 1, "the spare turn is given back");
    }

    #[test]
    fn a_failed_fragment_shader_library_fails_the_creation_and_waits_for_the_next() {
        let budget = Arc::new(Budget::new(4, 3));
        let _mine = waited_turn(&budget, ticket());
        let linked = linked();
        let makings = Makings::default();
        let failed = linked.made_with(
            || budget.spare(Urgency::Program),
            |part| match part {
                Part::FragmentShader => Err(internal("the driver refused")),
                _ => makings.made(part),
            },
        );
        assert!(failed.is_err());
        assert_eq!(
            makings.parts(),
            vec![Part::VertexInput, Part::PreRasterization]
        );
        assert!(!linked.0[Part::FragmentShader as usize].is_made());
        assert!(!linked.0[Part::FragmentOutput as usize].is_made());
        let made = linked
            .made_with(|| None, |part| makings.made(part))
            .unwrap();
        assert_eq!(made, Part::ALL.map(part_raw));
    }

    #[test]
    fn a_failed_pre_rasterization_library_fails_the_creation_first() {
        let budget = Arc::new(Budget::new(4, 3));
        let _mine = waited_turn(&budget, ticket());
        let linked = linked();
        let failed = linked.made_with(
            || budget.spare(Urgency::Program),
            |part| match part {
                Part::PreRasterization => Err(internal("pre-rasterization refused")),
                Part::FragmentShader => Err(internal("fragment refused")),
                _ => Ok(part_raw(part)),
            },
        );
        let error = format!("{:?}", failed.err().unwrap());
        assert!(error.contains("pre-rasterization refused"), "{error}");
    }

    #[test]
    fn without_a_spare_turn_the_libraries_are_made_in_order_here() {
        let linked = linked();
        let makings = Makings::default();
        linked
            .made_with(|| None, |part| makings.made(part))
            .unwrap();
        assert_eq!(makings.parts(), Part::ALL.to_vec());
        let here = std::thread::current().id();
        assert!(Part::ALL
            .iter()
            .all(|part| makings.thread_of(*part) == here));
    }

    #[test]
    fn a_made_pre_rasterization_library_asks_for_no_spare_turn() {
        let linked = linked();
        linked.0[Part::PreRasterization as usize]
            .made(|| Ok(part_raw(Part::PreRasterization)))
            .unwrap();
        let makings = Makings::default();
        linked
            .made_with(
                || panic!("a spare turn asked for"),
                |part| makings.made(part),
            )
            .unwrap();
        assert_eq!(
            makings.parts(),
            vec![
                Part::VertexInput,
                Part::FragmentShader,
                Part::FragmentOutput
            ]
        );
    }

    #[test]
    fn a_fragment_shader_library_being_made_gives_the_spare_turn_back() {
        let budget = Arc::new(Budget::new(4, 3));
        let _mine = waited_turn(&budget, ticket());
        let linked = linked();
        let claim = linked.0[Part::FragmentShader as usize].claim().unwrap();
        let (asked, has_asked) = mpsc::channel();
        std::thread::scope(|scope| {
            let creation = exit_guard::spawn_scoped(scope, Builder::new(), || {
                let _theirs = waited_turn(&budget, ticket());
                linked.made_with(
                    || {
                        let spare = budget.spare(Urgency::Program);
                        asked.send(()).unwrap();
                        spare
                    },
                    |part| Ok(part_raw(part)),
                )
            })
            .unwrap();
            has_asked.recv_timeout(WAIT).unwrap();
            claim.made(|| Ok(part_raw(Part::FragmentShader))).unwrap();
            assert_eq!(creation.join().unwrap().unwrap(), Part::ALL.map(part_raw));
        });
        assert_eq!(budget.census().0, 1);
    }

    #[test]
    fn a_library_is_made_once_by_its_first_caller() {
        let library = Library::new();
        assert!(!library.is_made());
        assert_eq!(library.made(|| Ok(raw(7))).unwrap(), raw(7));
        assert!(library.is_made());
        assert_eq!(
            library
                .made(|| panic!("a made library is made again"))
                .unwrap(),
            raw(7)
        );
    }

    #[test]
    fn a_failed_making_leaves_the_library_to_the_next_caller() {
        let library = Library::new();
        assert!(library
            .made(|| Err(internal("the driver refused")))
            .is_err());
        assert!(!library.is_made());
        assert_eq!(library.made(|| Ok(raw(9))).unwrap(), raw(9));
    }

    #[test]
    fn a_making_that_panics_leaves_the_library_to_the_next_caller() {
        let library = Library::new();
        std::thread::scope(|scope| {
            let library = &library;
            let maker = exit_guard::spawn_scoped(scope, Builder::new(), move || {
                library.made(|| panic!("a making panics"))
            })
            .unwrap();
            assert!(maker.join().is_err(), "the making panicked");
        });
        assert!(!library.is_made());
        assert_eq!(library.made(|| Ok(raw(11))).unwrap(), raw(11));
    }

    #[test]
    fn a_caller_waits_for_the_making_another_caller_began() {
        let library = Library::new();
        let (started, has_started) = mpsc::channel();
        let (finish, finishing) = mpsc::channel::<()>();
        let (returned, has_returned) = mpsc::channel();
        std::thread::scope(|scope| {
            let library = &library;
            let maker = exit_guard::spawn_scoped(scope, Builder::new(), move || {
                library.made(|| {
                    started.send(()).unwrap();
                    finishing.recv_timeout(WAIT).unwrap();
                    Ok(raw(13))
                })
            })
            .unwrap();
            has_started.recv_timeout(WAIT).unwrap();
            let waiter = exit_guard::spawn_scoped(scope, Builder::new(), move || {
                let made = library.made(|| panic!("a library being made is made again"));
                returned.send(()).unwrap();
                made
            })
            .unwrap();
            // The second caller is still waiting while the first one's making runs.
            assert!(has_returned
                .recv_timeout(Duration::from_millis(50))
                .is_err());
            finish.send(()).unwrap();
            assert_eq!(maker.join().unwrap().unwrap(), raw(13));
            assert_eq!(waiter.join().unwrap().unwrap(), raw(13));
        });
    }
}
