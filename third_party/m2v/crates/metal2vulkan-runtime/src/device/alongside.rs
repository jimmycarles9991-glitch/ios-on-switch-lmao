//! A part of one making run on a thread of the runtime's own beside the asking thread, which runs
//! the rest meanwhile (`pipeline/libraries.rs`: a creation's fragment shader library beside its
//! pre-rasterization library). The thread stands in for the asking thread as `compiles.rs`'s does:
//!
//! - **A turn of its own.** It makes with a spare turn of the process's making budget, handed to
//!   it (`budget::spare_turn`), so the budget still bounds the compiles in flight.
//! - **The asking thread's class**, raised to the user-initiated class on macOS as every making
//!   is (`compiles.rs`): a making for a background asker stays in the background.
//! - **The driver's measured diagnostics** the asking thread's call expects are expected there,
//!   and what was withheld is counted on the asking thread.
//! - **The program's code stays on the asking thread.** Messages for the device's sink the driver
//!   raises there are held and delivered on the asking thread once both parts are done.
//! - **The exit guard.** The thread makes inside a section of its own. Where no thread starts or
//!   the process is exiting, the asking thread makes that part itself after the rest.

use super::compiles::holding;
use crate::exit_guard;
use crate::pipeline::budget::Turn;

/// `there` on a thread of the runtime's own with `turn`, while this thread runs `here` (the module
/// docs); both results. `there` runs here, after `here`, where the thread does not start or the
/// process is exiting. A panic in either part unwinds this thread once both are done.
pub(crate) fn alongside<A: Send, B>(
    turn: Turn,
    there: impl FnOnce() -> A + Send,
    here: impl FnOnce() -> B,
) -> (A, B) {
    let expected = crate::driver_facts::expected_here();
    let class = Class::own();
    let mut asked = Some(there);
    let (made_there, made_here) = std::thread::scope(|scope| {
        let taken = &mut asked;
        let thread = exit_guard::spawn_scoped(
            scope,
            std::thread::Builder::new().name("metal2vulkan-alongside".into()),
            move || {
                let mut turn = turn;
                turn.hold_here();
                class.set_own();
                exit_guard::raise_to_user_initiated();
                let _busy = exit_guard::enter()?;
                let there = taken.take()?;
                Some(holding(|| crate::driver_facts::expecting(expected, there)))
            },
        );
        let made_here = here();
        let made_there = match thread {
            Ok(thread) => thread
                .join()
                .unwrap_or_else(|panic| std::panic::resume_unwind(panic)),
            Err(_) => None,
        };
        (made_there, made_here)
    });
    let made_there = match (made_there, asked) {
        (Some(((made, withheld), messages)), _) => {
            crate::driver_facts::withheld_for_here(withheld);
            for (sink, message) in messages {
                sink.deliver(&message);
            }
            made
        }
        (None, Some(there)) => there(),
        (None, None) => unreachable!("the runtime's thread took the part and made it"),
    };
    (made_there, made_here)
}

/// A thread's quality of service class and relative priority, where it has one.
#[cfg(target_vendor = "apple")]
#[derive(Clone, Copy)]
struct Class(Option<(libc::qos_class_t, libc::c_int)>);

#[cfg(target_vendor = "apple")]
impl Class {
    fn own() -> Class {
        let mut class = libc::qos_class_t::QOS_CLASS_UNSPECIFIED;
        let mut relative = 0;
        // SAFETY: the calling thread's own handle, and two out-parameters that live for the call.
        let read = unsafe {
            libc::pthread_get_qos_class_np(libc::pthread_self(), &mut class, &mut relative)
        };
        let unspecified = matches!(class, libc::qos_class_t::QOS_CLASS_UNSPECIFIED);
        Class((read == 0 && !unspecified).then_some((class, relative)))
    }

    fn set_own(self) {
        if let Some((class, relative)) = self.0 {
            // SAFETY: sets the calling thread's own class; no pointer.
            unsafe { libc::pthread_set_qos_class_self_np(class, relative) };
        }
    }
}

#[cfg(not(target_vendor = "apple"))]
struct Class;

#[cfg(not(target_vendor = "apple"))]
impl Class {
    fn own() -> Class {
        Class
    }

    fn set_own(self) {}
}

#[cfg(test)]
mod tests {
    use super::alongside;
    use crate::exit_guard;
    use crate::pipeline::budget::{holds_turn, ticket, waited_turn, Budget, Urgency};
    use std::sync::{mpsc, Arc};
    use std::thread::Builder;
    use std::time::Duration;

    const WAIT: Duration = Duration::from_secs(10);

    #[test]
    fn both_parts_run_at_once_the_other_holding_the_spare_turn() {
        let budget = Arc::new(Budget::new(4, 3));
        let _mine = waited_turn(&budget, ticket());
        let spare = budget.spare(Urgency::Program).unwrap();
        assert_eq!(budget.census().0, 2);
        let (there_started, there_has_started) = mpsc::channel();
        let (here_started, here_has_started) = mpsc::channel();
        let (there, here) = alongside(
            spare,
            move || {
                there_started.send(()).unwrap();
                here_has_started.recv_timeout(WAIT).unwrap();
                holds_turn()
            },
            move || {
                here_started.send(()).unwrap();
                there_has_started.recv_timeout(WAIT).unwrap();
                7
            },
        );
        assert!(there, "the runtime's thread holds the spare turn");
        assert_eq!(here, 7);
        assert!(holds_turn(), "the asking thread keeps its own turn");
        assert_eq!(budget.census().0, 1, "the spare turn is given back");
    }

    #[test]
    fn a_panic_in_the_other_part_unwinds_the_asking_thread_after_its_own() {
        let budget = Arc::new(Budget::new(4, 3));
        let (ran, has_run) = mpsc::channel();
        let unwound = std::thread::scope(|scope| {
            let asker = exit_guard::spawn_scoped(scope, Builder::new(), || {
                let _mine = waited_turn(&budget, ticket());
                let spare = budget.spare(Urgency::Program).unwrap();
                alongside(
                    spare,
                    || panic!("the other part panics"),
                    || ran.send(()).unwrap(),
                )
            })
            .unwrap();
            asker.join().is_err()
        });
        assert!(unwound);
        has_run.try_recv().unwrap();
        assert_eq!(budget.census().0, 0);
    }
}
