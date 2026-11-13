// SPDX-License-Identifier: MIT
use std::collections::HashMap;
use std::hash::Hash;
use std::sync::{Arc, Mutex, OnceLock};

type Pending<T> = Arc<OnceLock<Result<Arc<T>, String>>>;

pub(crate) struct Variants<K, T> {
    entries: Mutex<HashMap<K, Pending<T>>>,
}

impl<K, T> Default for Variants<K, T> {
    fn default() -> Self {
        Self {
            entries: Mutex::new(HashMap::new()),
        }
    }
}

impl<K: Clone + Eq + Hash, T> Variants<K, T> {
    pub fn get(&self, key: &K, make: impl FnOnce() -> Result<T, String>) -> Result<Arc<T>, String> {
        let pending = self
            .entries
            .lock()
            .map_err(|_| "native variant lock")?
            .entry(key.clone())
            .or_insert_with(|| Arc::new(OnceLock::new()))
            .clone();
        let result = pending.get_or_init(|| make().map(Arc::new)).clone();
        if result.as_ref().is_err_and(|why| why.starts_with("retry: ")) {
            let mut entries = self.entries.lock().map_err(|_| "native variant lock")?;
            // A waiter on the failed attempt must not remove a newer attempt.
            if entries.get(key).is_some_and(|entry| Arc::ptr_eq(entry, &pending)) {
                entries.remove(key);
            }
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::Variants;
    use std::sync::atomic::{AtomicUsize, Ordering};
    use std::sync::{Arc, Barrier};

    #[test]
    fn one_attempt_per_key() {
        let cache = Variants::<u32, u32>::default();
        let calls = AtomicUsize::new(0);
        let start = Barrier::new(16);
        let values = std::thread::scope(|scope| {
            let threads: Vec<_> = (0..16)
                .map(|_| {
                    scope.spawn(|| {
                        start.wait();
                        cache
                            .get(&7, || {
                                calls.fetch_add(1, Ordering::SeqCst);
                                Ok(42)
                            })
                            .unwrap()
                    })
                })
                .collect();
            threads.into_iter().map(|t| t.join().unwrap()).collect::<Vec<_>>()
        });
        assert_eq!(calls.load(Ordering::SeqCst), 1);
        assert!(values.iter().all(|value| Arc::ptr_eq(value, &values[0])));
    }

    #[test]
    fn independent_keys_compile_without_the_map_lock() {
        let cache = Variants::<u32, u32>::default();
        let compiling = Barrier::new(2);
        std::thread::scope(|scope| {
            for key in 0..2 {
                let cache = &cache;
                let compiling = &compiling;
                scope.spawn(move || {
                    assert_eq!(
                        *cache
                            .get(&key, || {
                                compiling.wait();
                                Ok(key)
                            })
                            .unwrap(),
                        key
                    );
                });
            }
        });
    }

    #[test]
    fn transient_failures_retry_but_permanent_refusals_remain() {
        let cache = Variants::<u32, u32>::default();
        assert_eq!(
            cache.get(&1, || Err("retry: deadline".into())).unwrap_err(),
            "retry: deadline"
        );
        let made = cache.get(&1, || Ok(99)).unwrap();
        assert!(Arc::ptr_eq(&made, &cache.get(&1, || panic!("cached success")).unwrap()));
        assert_eq!(cache.get(&2, || Err("unsupported".into())).unwrap_err(), "unsupported");
        assert_eq!(cache.get(&2, || panic!("cached refusal")).unwrap_err(), "unsupported");
    }
}
