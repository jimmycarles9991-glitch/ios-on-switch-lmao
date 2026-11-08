//! A binary archive's file: the compile layer's self-checking artifacts the creations added to it
//! used, each under its kind, key and profile, and the archive key (`metal_key`) of each descriptor
//! added.
//!
//! ```text
//! magic      8 bytes  "M2VBARCH"
//! version    u32      1
//! keys       u32      their count, then each key's 32 bytes, ascending
//! entries    u32      their count, then each entry, ascending by (kind, key, profile):
//!              kind     u32       a translated function 1, a compute pipeline 2, a render
//!                                 pipeline 4 (the shader cache's entry kinds)
//!              key      32 bytes  the function's or pipeline's key
//!              profile  32 bytes  the fingerprint of the profile it was compiled for
//!              length   u64       then that many bytes: the artifact's encoding
//! checksum   32 bytes SHA-256 of every byte before it
//! ```
//!
//! Integers are little-endian. Bytes that are not exactly this (another magic or version, a kind
//! not listed, a count or length past the end, bytes after the checksum, a checksum that does
//! not hold, keys or entries out of order or repeated) are not an archive. Nothing is allocated
//! for a count before the bytes it counts are there.
//!
//! A metallib is an archive too, holding nothing for this device: thin (`MTLB`), or universal (a
//! big-endian fat file of AIR64 slices that are one and binaries for Apple's GPUs), as Apple's
//! frameworks ship theirs and QuartzCore and CoreUI open them as their archives. Metal loads such
//! a file on a GPU it holds no binary for with a count of 0, and every lookup misses (measured on
//! an M4: iOS 17's QuartzCore file, AIR64 and applegpu_g9g, either slice alone, and a thin AIR
//! metallib). Only its header, slice table and each AIR64 slice's magic are read.
//!
//! An archive's file is read whole, and written through a temporary file beside it and a rename, so a
//! reader never sees part of one. The errors are Metal's (measured on an M3 Max and an M4): a
//! file that is missing or not an archive, or a missing directory to write into, is
//! `InvalidFile`; any other failure to write (a directory at the path, for one) is `Write`.

use crate::error::{BinaryArchiveError, Error};
use crate::shader_cache::EntryKind;
use metal2vulkan::cache::Hash256;
use sha2::{Digest, Sha256};
use std::collections::{BTreeMap, BTreeSet};
use std::io::{Read, Seek, SeekFrom, Write as _};
use std::path::{Path, PathBuf};
use std::sync::Arc;

const MAGIC: &[u8; 8] = b"M2VBARCH";
const VERSION: u32 = 1;
const METALLIB_MAGIC: [u8; 4] = *b"MTLB";
const UNIVERSAL_MAGIC: u32 = 0xcafe_babe;
const AIR64_CPU_TYPE: u32 = 0x0100_0017;
const APPLE_GPU_CPU_TYPE: u32 = 0x0100_0013;
const UNIVERSAL_SLICE_LEN: usize = 20;
const UNIVERSAL_SLICES_MAX: usize = 64;
const KINDS: [EntryKind; 3] = [
    EntryKind::Function,
    EntryKind::ComputePipeline,
    EntryKind::RenderPipeline,
];

/// An artifact's place in an archive.
#[derive(Clone, Copy, Debug, PartialEq, Eq, PartialOrd, Ord, Hash)]
pub(crate) struct Place {
    pub(crate) kind: u32,
    pub(crate) key: Hash256,
    pub(crate) profile: Hash256,
}

impl Place {
    /// The place of the artifact of `kind` under `key` for the profile of fingerprint
    /// `profile`.
    pub(crate) fn new(kind: EntryKind, key: Hash256, profile: Hash256) -> Place {
        Place {
            kind: kind as u32,
            key,
            profile,
        }
    }
}

/// What an archive holds.
#[derive(Clone, Debug, Default, PartialEq, Eq)]
pub(crate) struct Contents {
    /// The archive keys of the descriptors added.
    pub(crate) keys: BTreeSet<Hash256>,
    /// The artifacts' encodings by place.
    pub(crate) entries: BTreeMap<Place, Arc<[u8]>>,
}

impl Contents {
    /// Whether nothing was added.
    pub(crate) fn is_empty(&self) -> bool {
        self.keys.is_empty() && self.entries.is_empty()
    }
}

/// The file of `contents`.
pub(crate) fn encode(contents: &Contents) -> Vec<u8> {
    let bytes: usize = contents
        .entries
        .values()
        .map(|entry| entry.len() + 76)
        .sum();
    let mut out = Vec::with_capacity(52 + contents.keys.len() * 32 + bytes);
    out.extend_from_slice(MAGIC);
    out.extend_from_slice(&VERSION.to_le_bytes());
    out.extend_from_slice(&(contents.keys.len() as u32).to_le_bytes());
    for key in &contents.keys {
        out.extend_from_slice(key);
    }
    out.extend_from_slice(&(contents.entries.len() as u32).to_le_bytes());
    for (place, entry) in &contents.entries {
        out.extend_from_slice(&place.kind.to_le_bytes());
        out.extend_from_slice(&place.key);
        out.extend_from_slice(&place.profile);
        out.extend_from_slice(&(entry.len() as u64).to_le_bytes());
        out.extend_from_slice(entry);
    }
    let checksum: Hash256 = Sha256::digest(&out).into();
    out.extend_from_slice(&checksum);
    out
}

/// The contents of `bytes`, or `None` when they are not an archive.
pub(crate) fn decode(bytes: &[u8]) -> Option<Contents> {
    let end = bytes.len().checked_sub(32)?;
    let checksum: Hash256 = Sha256::digest(&bytes[..end]).into();
    if bytes[end..] != checksum {
        return None;
    }
    let mut reader = Reader(&bytes[..end]);
    if reader.take(8)? != MAGIC || reader.u32()? != VERSION {
        return None;
    }
    let mut contents = Contents::default();
    let keys = reader.u32()? as usize;
    if keys.checked_mul(32)? > reader.0.len() {
        return None;
    }
    for _ in 0..keys {
        let key = reader.hash()?;
        if contents.keys.last().is_some_and(|last| *last >= key) {
            return None;
        }
        contents.keys.insert(key);
    }
    let entries = reader.u32()?;
    for _ in 0..entries {
        let kind = reader.u32()?;
        if !KINDS.iter().any(|known| *known as u32 == kind) {
            return None;
        }
        let place = Place {
            kind,
            key: reader.hash()?,
            profile: reader.hash()?,
        };
        let length = usize::try_from(reader.u64()?).ok()?;
        let entry = reader.take(length)?;
        if contents
            .entries
            .last_key_value()
            .is_some_and(|(last, _)| *last >= place)
        {
            return None;
        }
        contents.entries.insert(place, Arc::from(entry));
    }
    reader.0.is_empty().then_some(contents)
}

/// The bytes not read yet.
struct Reader<'a>(&'a [u8]);

impl<'a> Reader<'a> {
    fn take(&mut self, count: usize) -> Option<&'a [u8]> {
        if count > self.0.len() {
            return None;
        }
        let (taken, rest) = self.0.split_at(count);
        self.0 = rest;
        Some(taken)
    }

    fn u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.take(4)?.try_into().ok()?))
    }

    fn u64(&mut self) -> Option<u64> {
        Some(u64::from_le_bytes(self.take(8)?.try_into().ok()?))
    }

    fn hash(&mut self) -> Option<Hash256> {
        self.take(32)?.try_into().ok()
    }
}

/// The archive in the file `path`.
pub(crate) fn read(path: &Path) -> Result<Contents, Error> {
    std::fs::File::open(path)
        .ok()
        .and_then(read_file)
        .ok_or_else(|| invalid_file(path))
}

fn read_file(mut file: std::fs::File) -> Option<Contents> {
    let mut head = [0u8; 8];
    file.read_exact(&mut head).ok()?;
    if head == *MAGIC {
        let mut bytes = head.to_vec();
        file.read_to_end(&mut bytes).ok()?;
        return decode(&bytes);
    }
    is_metallib(&mut file, head).then(Contents::default)
}

/// Whether the file whose first eight bytes are `head` is a metallib, thin or universal.
fn is_metallib(file: &mut std::fs::File, head: [u8; 8]) -> bool {
    if head[..4] == METALLIB_MAGIC {
        return true;
    }
    let word = |bytes: &[u8], at: usize| {
        u32::from_be_bytes([bytes[at], bytes[at + 1], bytes[at + 2], bytes[at + 3]])
    };
    if word(&head, 0) != UNIVERSAL_MAGIC {
        return false;
    }
    let count = word(&head, 4) as usize;
    if count == 0 || count > UNIVERSAL_SLICES_MAX {
        return false;
    }
    let mut table = vec![0u8; count * UNIVERSAL_SLICE_LEN];
    let Ok(length) = file.metadata().map(|m| m.len()) else {
        return false;
    };
    if file.read_exact(&mut table).is_err() {
        return false;
    }
    table.chunks_exact(UNIVERSAL_SLICE_LEN).all(|slice| {
        let (offset, size) = (u64::from(word(slice, 8)), u64::from(word(slice, 12)));
        let mut magic = [0u8; 4];
        offset + size <= length
            && match word(slice, 0) {
                APPLE_GPU_CPU_TYPE => true,
                AIR64_CPU_TYPE => {
                    file.seek(SeekFrom::Start(offset)).is_ok()
                        && file.read_exact(&mut magic).is_ok()
                        && magic == METALLIB_MAGIC
                }
                _ => false,
            }
    })
}

/// Write `bytes`, an archive's encoding, to the file `url`, replacing what is there.
pub(crate) fn write(bytes: &[u8], url: &Path) -> Result<(), Error> {
    write_file(bytes, url).map_err(|failure| match failure {
        WriteFailure::NoDirectory => invalid_file(url),
        WriteFailure::Io(error) => Error::BinaryArchive(BinaryArchiveError::Write {
            path: url.to_path_buf(),
            kind: error.kind(),
            message: error.to_string(),
        }),
    })
}

/// Why [`write_file`] could not write a file.
pub(crate) enum WriteFailure {
    /// The directory to write into is missing.
    NoDirectory,
    /// Writing failed (a directory at the path, for one).
    Io(std::io::Error),
}

/// Write `bytes` to the file `url`, replacing what is there, through a temporary file beside it
/// and a rename, so a reader never sees part of it: a binary archive's file, and a dynamic
/// library's (`dynamic_library.rs`).
pub(crate) fn write_file(bytes: &[u8], url: &Path) -> Result<(), WriteFailure> {
    let directory = match url.parent() {
        Some(parent) if !parent.as_os_str().is_empty() => parent.to_path_buf(),
        _ => PathBuf::from("."),
    };
    if !directory.is_dir() {
        return Err(WriteFailure::NoDirectory);
    }
    if url.is_dir() {
        return Err(WriteFailure::Io(std::io::Error::new(
            std::io::ErrorKind::IsADirectory,
            "a directory is at the path",
        )));
    }
    let mut temporary = Temporary::beside(&directory, url).map_err(WriteFailure::Io)?;
    temporary
        .file
        .write_all(bytes)
        .and_then(|()| temporary.file.sync_all())
        .and_then(|()| std::fs::rename(&temporary.path, url))
        .map_err(WriteFailure::Io)?;
    temporary.renamed = true;
    Ok(())
}

/// `BinaryArchive(InvalidFile)` of `path`.
fn invalid_file(path: &Path) -> Error {
    Error::BinaryArchive(BinaryArchiveError::InvalidFile {
        path: path.to_path_buf(),
    })
}

/// A temporary file beside the file it writes, removed when dropped unless renamed onto it.
struct Temporary {
    path: PathBuf,
    file: std::fs::File,
    renamed: bool,
}

impl Temporary {
    /// A new temporary file in `directory` for writing `url`, named after it, the process and a
    /// counter.
    fn beside(directory: &Path, url: &Path) -> std::io::Result<Temporary> {
        use std::sync::atomic::{AtomicU64, Ordering};
        static NEXT: AtomicU64 = AtomicU64::new(0);
        let name = url
            .file_name()
            .map_or_else(|| "archive".into(), |name| name.to_string_lossy());
        let path = directory.join(format!(
            ".{name}.{}.{}.tmp",
            std::process::id(),
            NEXT.fetch_add(1, Ordering::Relaxed)
        ));
        let file = std::fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(&path)?;
        Ok(Temporary {
            path,
            file,
            renamed: false,
        })
    }
}

impl Drop for Temporary {
    fn drop(&mut self) {
        if !self.renamed {
            let _ = std::fs::remove_file(&self.path);
        }
    }
}
