//! Compiled stages a process keeps by their [`FunctionCompileKey`] and their libraries' scope, once
//! [`keep_compiled_stages`] turns keeping on (the system shader compiler does; nothing else keeps any). A stage
//! another pipeline of the process compiles under the same key and scope, or another form of the same pipeline, is
//! taken as compiled: a compile is a function of its key, which names its bitcode by its `HASH` tag. A tag is taken
//! as its library states it, so only a sealed library's compiles (scope 0) are shared across libraries; every other
//! library's are its own (`Library::scope`). What the compile charged its meter is kept with it and charged again at each use,
//! so a pipeline over the work or ledger caps is refused whether its stages were kept or not. A
//! failed compile is not kept.

use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicUsize, Ordering};
use std::sync::{Arc, Mutex, MutexGuard};

use crate::budget::{Charged, Meter};
use crate::error::Error;
use crate::library::{FunctionArtifact, FunctionCompileKey};
use crate::pipeline::native::NativeFunction;

/// The bytes each kind of kept stage may hold (0: none kept).
static LIMIT: AtomicUsize = AtomicUsize::new(0);

/// A kept stage's bytes past its own: the key, the slot, its order entry.
const ENTRY_BYTES: usize = 256;

struct Made<T> {
    value: T,
    charged: Charged,
}

/// One key's compile. A second compile of the key waits on the first's lock and takes what it
/// made: the shader compiler's workers ask the same stage at once in a burst of a pipeline's
/// formats, and the lock (an owner the kernel lends the waiter's priority to) is held for the
/// one compile.
type Slot<T> = Arc<Mutex<Option<Made<T>>>>;

/// A compile key and the scope it is kept under
type Scoped = (FunctionCompileKey, u64);

struct Kept<T> {
    slots: HashMap<Scoped, (Slot<T>, usize)>,
    /// The made keys, the most recently used last.
    order: VecDeque<Scoped>,
    bytes: usize,
}

impl<T> Kept<T> {
    fn new() -> Kept<T> {
        Kept {
            slots: HashMap::new(),
            order: VecDeque::new(),
            bytes: 0,
        }
    }
}

/// The kept stages of each kind, shared by every thread of the process by one rule: an asker finds
/// or adds its key's slot under the store's lock, then holds the slot's lock for its compile, so
/// the first asker of a key compiles it and a concurrent or later asker waits and takes a clone of
/// the same value; a failed compile is not kept (the next asker compiles again). A kept value is
/// never written again (a portable stage is shared behind its `Arc`). The store's lock is never
/// held across a compile or a wait, and no thread holds two slots.
static ARTIFACTS: Mutex<Option<Kept<Arc<FunctionArtifact>>>> = Mutex::new(None);
static NATIVE: Mutex<Option<Kept<NativeFunction>>> = Mutex::new(None);

fn lock<T>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(|poisoned| poisoned.into_inner())
}

/// Keep up to `bytes` of compiled portable stages and as many of native ones; 0 keeps none and
/// lets go of what was kept.
pub fn keep_compiled_stages(bytes: usize) {
    LIMIT.store(bytes, Ordering::Relaxed);
    if bytes == 0 {
        *lock(&ARTIFACTS) = None;
        *lock(&NATIVE) = None;
    }
}

/// The stages kept, of both kinds, and their bytes.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct KeptStages {
    pub entries: u64,
    pub bytes: u64,
}

pub fn kept_compiled_stages() -> KeptStages {
    let mut out = KeptStages::default();
    for (entries, bytes) in [
        lock(&ARTIFACTS).as_ref().map(|k| (k.order.len(), k.bytes)),
        lock(&NATIVE).as_ref().map(|k| (k.order.len(), k.bytes)),
    ]
    .into_iter()
    .flatten()
    {
        out.entries += entries as u64;
        out.bytes += bytes as u64;
    }
    out
}

/// The portable stage of `key`, kept or made by `make`: its bytes are its SPIR-V's and relocations'.
pub(crate) fn artifact(
    key: FunctionCompileKey,
    scope: u64,
    m: &mut Meter,
    make: impl FnOnce(&mut Meter) -> Result<Arc<FunctionArtifact>, Error>,
) -> Result<Arc<FunctionArtifact>, Error> {
    kept(&ARTIFACTS, (key, scope), m, make, |a, _| a.module.footprint())
}

/// The native stage of `key`, kept or made by `make`: its typed module has no measure of its own, so its bytes are
/// what its compile left charged.
pub(crate) fn native(
    key: FunctionCompileKey,
    scope: u64,
    m: &mut Meter,
    make: impl FnOnce(&mut Meter) -> Result<NativeFunction, Error>,
) -> Result<NativeFunction, Error> {
    kept(&NATIVE, (key, scope), m, make, |_, charged| {
        usize::try_from(charged.kept.max(0)).unwrap_or(usize::MAX)
    })
}

/// `size`: the bytes a made stage holds
fn kept<T: Clone>(
    store: &Mutex<Option<Kept<T>>>,
    key: Scoped,
    m: &mut Meter,
    make: impl FnOnce(&mut Meter) -> Result<T, Error>,
    size: impl FnOnce(&T, &Charged) -> usize,
) -> Result<T, Error> {
    let limit = LIMIT.load(Ordering::Relaxed);
    if limit == 0 {
        return make(m);
    }
    let slot = {
        let mut held = lock(store);
        let k = held.get_or_insert_with(Kept::new);
        Arc::clone(
            &k.slots
                .entry(key)
                .or_insert_with(|| (Arc::new(Mutex::new(None)), 0))
                .0,
        )
    };
    let mut made = lock(&slot);
    if let Some(found) = made.as_ref() {
        m.replay(&found.charged)?;
        let value = found.value.clone();
        drop(made);
        used(store, key);
        return Ok(value);
    }
    let mark = m.mark();
    let result = make(m);
    let charged = m.charged_since(mark);
    match result {
        Ok(value) => {
            let bytes = size(&value, &charged).saturating_add(ENTRY_BYTES);
            *made = Some(Made {
                value: value.clone(),
                charged,
            });
            drop(made);
            record(store, key, &slot, bytes, limit);
            Ok(value)
        }
        Err(error) => {
            drop(made);
            forget(store, key, &slot);
            Err(error)
        }
    }
}

fn used<T>(store: &Mutex<Option<Kept<T>>>, key: Scoped) {
    if let Some(k) = lock(store).as_mut() {
        if let Some(at) = k.order.iter().position(|o| *o == key) {
            k.order.remove(at);
            k.order.push_back(key);
        }
    }
}

fn record<T>(
    store: &Mutex<Option<Kept<T>>>,
    key: Scoped,
    slot: &Slot<T>,
    bytes: usize,
    limit: usize,
) {
    let mut held = lock(store);
    let Some(k) = held.as_mut() else {
        return;
    };
    match k.slots.get_mut(&key) {
        Some((kept, size)) if Arc::ptr_eq(kept, slot) && *size == 0 => *size = bytes,
        _ => return,
    }
    // a stage past the whole bound is not kept at the others' cost
    if bytes > limit {
        k.slots.remove(&key);
        return;
    }
    k.bytes += bytes;
    k.order.push_back(key);
    while k.bytes > limit && k.order.len() > 1 {
        let Some(oldest) = k.order.pop_front() else {
            break;
        };
        if let Some((_, size)) = k.slots.remove(&oldest) {
            k.bytes -= size;
        }
    }
}

fn forget<T>(store: &Mutex<Option<Kept<T>>>, key: Scoped, slot: &Slot<T>) {
    if let Some(k) = lock(store).as_mut() {
        if k.slots
            .get(&key)
            .is_some_and(|(kept, size)| Arc::ptr_eq(kept, slot) && *size == 0)
        {
            k.slots.remove(&key);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::budget::Budget;
    use crate::error::Limit;
    use std::sync::atomic::AtomicU32;

    static NUMBERS: Mutex<Option<Kept<u32>>> = Mutex::new(None);

    /// The numbers' stages hold what their compile left charged
    fn kept_numbers(
        store: &Mutex<Option<Kept<u32>>>,
        key: Scoped,
        m: &mut Meter,
        make: impl FnOnce(&mut Meter) -> Result<u32, Error>,
    ) -> Result<u32, Error> {
        kept(store, key, m, make, |_, charged| usize::try_from(charged.kept.max(0)).unwrap_or(0))
    }
    /// The tests share LIMIT and the store: one at a time
    static SERIAL: Mutex<()> = Mutex::new(());

    fn key(n: u8) -> Scoped {
        (FunctionCompileKey::new([n; 32]), 0)
    }

    fn fresh(limit: usize) -> MutexGuard<'static, ()> {
        let serial = lock(&SERIAL);
        LIMIT.store(limit, Ordering::Relaxed);
        *lock(&NUMBERS) = None;
        serial
    }

    /// A compile of `value` that counts itself, works 30 units and keeps 100 of 600 bytes
    fn compile(value: u32, count: &AtomicU32) -> impl FnOnce(&mut Meter) -> Result<u32, Error> + '_ {
        move |m| {
            count.fetch_add(1, Ordering::Relaxed);
            m.tick(30)?;
            m.charge(600)?;
            m.release(500);
            Ok(value)
        }
    }

    #[test]
    fn a_kept_stage_is_made_once_and_charges_each_use_what_its_compile_charged() {
        let _serial = fresh(1 << 20);
        let count = AtomicU32::new(0);
        let mut cold = Meter::with_caps(&Budget::default(), 1000, 10_000);
        assert_eq!(kept_numbers(&NUMBERS, key(1), &mut cold, compile(7, &count)), Ok(7));
        let mut warm = Meter::with_caps(&Budget::default(), 1000, 10_000);
        assert_eq!(kept_numbers(&NUMBERS, key(1), &mut warm, compile(8, &count)), Ok(7));
        assert_eq!(count.load(Ordering::Relaxed), 1);
        assert_eq!(
            (warm.work(), warm.ledger(), warm.ledger_peak()),
            (cold.work(), cold.ledger(), cold.ledger_peak())
        );
    }

    /// An app library stating a sealed function's tag compiles its own stage: the sealed one's is not handed to it,
    /// nor its to the sealed library's next compile
    #[test]
    fn a_stage_is_kept_apart_for_each_scope() {
        let _serial = fresh(1 << 20);
        let count = AtomicU32::new(0);
        let (compile_key, _) = key(9);
        let mut m = Meter::with_caps(&Budget::default(), 1000, 10_000);
        assert_eq!(kept_numbers(&NUMBERS, (compile_key, 0), &mut m, compile(1, &count)), Ok(1));
        assert_eq!(kept_numbers(&NUMBERS, (compile_key, 7), &mut m, compile(2, &count)), Ok(2));
        assert_eq!(kept_numbers(&NUMBERS, (compile_key, 0), &mut m, compile(3, &count)), Ok(1));
        assert_eq!(kept_numbers(&NUMBERS, (compile_key, 7), &mut m, compile(4, &count)), Ok(2));
        assert_eq!(count.load(Ordering::Relaxed), 2);
    }

    #[derive(Debug)]
    struct Held(Vec<u8>);

    impl crate::library::HeldBytes for Held {
        fn bytes(&self) -> &[u8] {
            &self.0
        }
    }

    /// The scope a stage is kept under, from its libraries: one sealed library's functions share it; two unsealed
    /// libraries stating the same `HASH` (one bitcode under it here) get scopes of their own; a stage reading two
    /// unsealed libraries is kept by neither
    #[test]
    fn a_stages_scope_shares_only_what_sealed_libraries_state() {
        use crate::library::Library;
        use crate::pipeline::compile::{stage_scope, DynamicOrder};
        use crate::pipeline::descriptor::{LinkedFunction, LinkedFunctions};
        use metal2vulkan_devtools::metallib::{metallib, MtlbFunction};
        let bytes = metallib(&[
            MtlbFunction {
                name: "k",
                type_code: 2,
                bitcode: b"one function's bitcode".to_vec(),
                constants: vec![],
                public: vec![],
                tags: vec![],
            },
            MtlbFunction {
                name: "v",
                type_code: 4,
                bitcode: b"a visible function's bitcode".to_vec(),
                constants: vec![],
                public: vec![],
                tags: vec![],
            },
        ]);
        let sealed = |b: &Vec<u8>| {
            Library::from_held_metallib(Arc::new(Held(b.clone())), true).expect("sealed library")
        };
        let (system, also_system) = (sealed(&bytes), sealed(&bytes));
        let app = Library::from_metallib(&bytes).expect("app library");
        let other_app = Library::from_metallib(&bytes).expect("another app's library");
        let none = LinkedFunctions::default();
        let dynamic = DynamicOrder::default();
        let scope = |l: &Library, linked: &LinkedFunctions| {
            stage_scope(&l.function("k").expect("k"), linked, &dynamic)
        };
        assert_eq!(scope(&system, &none), Some(0));
        assert_eq!(scope(&also_system, &none), Some(0));
        let (a, b) = (scope(&app, &none), scope(&other_app, &none));
        assert!(a.is_some_and(|s| s != 0) && b.is_some_and(|s| s != 0) && a != b, "{a:?} {b:?}");
        let mut own = LinkedFunctions::default();
        own.functions
            .push(LinkedFunction::new(app.function("v").expect("v")));
        assert_eq!(scope(&app, &own), a, "an app's function linking its own library");
        assert_eq!(scope(&system, &own), a, "a system function linking an app's");
        assert_eq!(scope(&other_app, &own), None, "two apps' libraries");

        let app_scope = Library::new_scope();
        let in_app = |b: &Vec<u8>| {
            Library::from_held_metallib_in(Arc::new(Held(b.clone())), app_scope).expect("app library")
        };
        let (read, read_again) = (in_app(&bytes), in_app(&bytes));
        assert_eq!(scope(&read, &none), Some(app_scope));
        assert_eq!(scope(&read_again, &none), Some(app_scope), "the app's library read again");
        let mut again = LinkedFunctions::default();
        again
            .functions
            .push(LinkedFunction::new(read_again.function("v").expect("v")));
        assert_eq!(scope(&read, &again), Some(app_scope), "two reads of one app's libraries");
        assert_eq!(scope(&app, &again), None, "a library of another scope beside them");
        assert!(Library::from_held_metallib_in(Arc::new(Held(bytes.clone())), 0).is_err());
    }

    #[test]
    fn a_use_over_the_caps_is_refused_as_its_compile_would_be() {
        let _serial = fresh(1 << 20);
        let count = AtomicU32::new(0);
        let mut roomy = Meter::with_caps(&Budget::default(), 1000, 10_000);
        kept_numbers(&NUMBERS, key(2), &mut roomy, compile(7, &count)).unwrap();
        let mut full = Meter::with_caps(&Budget::default(), 1000, 1000);
        full.charge(500).unwrap();
        assert_eq!(
            kept_numbers(&NUMBERS, key(2), &mut full, compile(7, &count)),
            Err(Error::LimitExceeded(Limit::LedgerBytes))
        );
        let mut busy = Meter::with_caps(&Budget::default(), 40, 10_000);
        busy.tick(20).unwrap();
        assert_eq!(
            kept_numbers(&NUMBERS, key(2), &mut busy, compile(7, &count)),
            Err(Error::LimitExceeded(Limit::WorkUnits))
        );
        assert_eq!(count.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn a_failed_compile_is_not_kept() {
        let _serial = fresh(1 << 20);
        let mut m = Meter::new(&Budget::default());
        let failed = kept_numbers(&NUMBERS, key(3), &mut m, |_| {
            Err(Error::LimitExceeded(Limit::WorkUnits))
        });
        assert!(failed.is_err());
        let count = AtomicU32::new(0);
        assert_eq!(kept_numbers(&NUMBERS, key(3), &mut m, compile(9, &count)), Ok(9));
        assert_eq!(count.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn past_its_bytes_the_least_recently_used_stage_goes() {
        let _serial = fresh(2 * (100 + ENTRY_BYTES));
        let count = AtomicU32::new(0);
        let mut m = Meter::new(&Budget::default());
        for n in [4, 5] {
            kept_numbers(&NUMBERS, key(n), &mut m, compile(u32::from(n), &count)).unwrap();
        }
        kept_numbers(&NUMBERS, key(4), &mut m, compile(4, &count)).unwrap();
        kept_numbers(&NUMBERS, key(6), &mut m, compile(6, &count)).unwrap();
        assert_eq!(count.load(Ordering::Relaxed), 3);
        kept_numbers(&NUMBERS, key(4), &mut m, compile(4, &count)).unwrap();
        assert_eq!(count.load(Ordering::Relaxed), 3, "4 was used last, so 5 went");
        kept_numbers(&NUMBERS, key(5), &mut m, compile(5, &count)).unwrap();
        assert_eq!(count.load(Ordering::Relaxed), 4);
    }

    #[test]
    fn a_stage_past_the_whole_bound_is_not_kept_and_evicts_nothing() {
        let _serial = fresh(100 + ENTRY_BYTES + 50);
        let count = AtomicU32::new(0);
        let mut m = Meter::new(&Budget::default());
        kept_numbers(&NUMBERS, key(9), &mut m, compile(9, &count)).unwrap();
        kept_numbers(&NUMBERS, key(10), &mut m, |m| {
            m.charge(1000)?;
            Ok(10)
        })
        .unwrap();
        kept_numbers(&NUMBERS, key(9), &mut m, compile(9, &count)).unwrap();
        assert_eq!(count.load(Ordering::Relaxed), 1, "9 stays kept");
        let made = AtomicU32::new(0);
        for _ in 0..2 {
            kept_numbers(&NUMBERS, key(10), &mut m, |m| {
                made.fetch_add(1, Ordering::Relaxed);
                m.charge(1000)?;
                Ok(10)
            })
            .unwrap();
        }
        assert_eq!(made.load(Ordering::Relaxed), 2, "10 is never kept");
    }

    #[test]
    fn a_compile_asked_twice_at_once_runs_once() {
        let _serial = fresh(1 << 20);
        let count = AtomicU32::new(0);
        std::thread::scope(|s| {
            for _ in 0..4 {
                s.spawn(|| {
                    let mut m = Meter::new(&Budget::default());
                    let made = kept_numbers(&NUMBERS, key(7), &mut m, |m| {
                        std::thread::sleep(std::time::Duration::from_millis(50));
                        compile(11, &count)(m)
                    });
                    assert_eq!(made, Ok(11));
                });
            }
        });
        assert_eq!(count.load(Ordering::Relaxed), 1);
    }

    #[test]
    fn nothing_is_kept_unless_asked_for() {
        let _serial = fresh(0);
        let count = AtomicU32::new(0);
        let mut m = Meter::new(&Budget::default());
        kept_numbers(&NUMBERS, key(8), &mut m, compile(1, &count)).unwrap();
        kept_numbers(&NUMBERS, key(8), &mut m, compile(1, &count)).unwrap();
        assert_eq!(count.load(Ordering::Relaxed), 2);
    }
}
