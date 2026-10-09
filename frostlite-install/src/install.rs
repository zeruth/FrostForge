//! Lays the selected build out as a Battle.net install:
//!
//!   .build.info                      active build, tags, CDN
//!   Data/config/xx/yy/<hash>         build and CDN configs
//!   Data/indices/<archive>.index     the CDN archive indexes
//!   Data/data/data.###, *.idx        local CASC storage, BLTE blobs stored as served
//!   _classic_/...                    loose install-manifest files, decoded
//!   _classic_/.flavor.info

use crate::cdn::{Cdn, blte_ekey};
use crate::manifests::{Manifests, blte_decode, selected, selection_masks, work_dir};
use crate::{BUILD_CONFIG, BUILD_INFO_TAGS, CDN_CONFIG, FLAVOR_DIR, PRODUCT, SELECT_TAGS, VERSION};
use anyhow::{Context, Result, anyhow, bail};
use cascette_client_storage::index::IndexManager;
use cascette_client_storage::storage::local_header::{LOCAL_HEADER_SIZE, LocalHeader};
use cascette_crypto::EncodingKey;
use cascette_formats::archive::ArchiveIndex;
use parking_lot::Mutex;
use rayon::prelude::*;
use std::collections::{BTreeMap, HashMap, HashSet};
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Read, Seek, SeekFrom, Write};
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::time::{Duration, Instant};

type Key = [u8; 16];

/// A data.### segment holds at most 2^30 bytes: the .idx location packs a 30-bit offset.
const SEGMENT_SIZE: u64 = 1 << 30;
/// Coalesce blobs of one archive into a single range request across gaps this small.
const MAX_GAP: u64 = 1 << 20;
const MAX_RANGE: u64 = 32 << 20;
const THREADS: usize = 16;

fn key(hex_str: &str) -> Result<Key> {
    let v = hex::decode(hex_str)?;
    v.try_into().map_err(|_| anyhow!("key {hex_str} is not 16 bytes"))
}

/// Where a stored blob lives: segment, offset of its local header, size including that header.
#[derive(Clone, Copy)]
struct Loc {
    seg: u16,
    off: u32,
    size: u32,
}

/// Appends blobs to data.### and journals each one, so a rerun continues where the
/// last one stopped.
struct Writer {
    dir: PathBuf,
    seg: u16,
    off: u64,
    file: File,
    journal: File,
    stored: HashMap<Key, Loc>,
}

impl Writer {
    fn open(root: &Path) -> Result<Self> {
        let dir = root.join("Data").join("data");
        std::fs::create_dir_all(&dir)?;
        let jpath = work_dir(root).join("journal.txt");
        std::fs::create_dir_all(jpath.parent().unwrap())?;

        let mut stored = HashMap::new();
        let mut ends: BTreeMap<u16, u64> = BTreeMap::new();
        if let Ok(f) = File::open(&jpath) {
            for line in BufReader::new(f).lines() {
                let line = line?;
                let p: Vec<&str> = line.split_whitespace().collect();
                if p.len() != 4 {
                    break; // a torn last line: everything after it is discarded below
                }
                let loc = Loc { seg: p[1].parse()?, off: p[2].parse()?, size: p[3].parse()? };
                let end = u64::from(loc.off) + u64::from(loc.size);
                let e = ends.entry(loc.seg).or_default();
                *e = (*e).max(end);
                stored.insert(key(p[0])?, loc);
            }
        }
        // Rewrite the journal clean, so a torn line never survives into the next run.
        {
            let mut j = File::create(&jpath)?;
            for (k, l) in &stored {
                writeln!(j, "{} {} {} {}", hex::encode(k), l.seg, l.off, l.size)?;
            }
        }
        let (seg, off) = ends.iter().next_back().map(|(&s, &e)| (s, e)).unwrap_or((0, 0));
        // Drop anything written after the last journaled blob, and any later segment.
        for entry in std::fs::read_dir(&dir)? {
            let entry = entry?;
            let name = entry.file_name().to_string_lossy().to_string();
            if let Some(n) = name.strip_prefix("data.").and_then(|n| n.parse::<u16>().ok()) {
                if n > seg {
                    std::fs::remove_file(entry.path())?;
                }
            }
        }
        let path = dir.join(format!("data.{seg:03}"));
        let file = OpenOptions::new().create(true).truncate(false).read(true).write(true).open(&path)?;
        file.set_len(off)?;
        let journal = OpenOptions::new().append(true).open(&jpath)?;
        Ok(Self { dir, seg, off, file, journal, stored })
    }

    fn store(&mut self, k: &Key, blob: &[u8]) -> Result<()> {
        if self.stored.contains_key(k) {
            return Ok(());
        }
        let total = (LOCAL_HEADER_SIZE + blob.len()) as u64;
        if total > SEGMENT_SIZE {
            bail!("blob {} is larger than a segment", hex::encode(k));
        }
        if self.off + total > SEGMENT_SIZE {
            self.file.sync_all()?;
            self.seg += 1;
            self.off = 0;
            self.file = OpenOptions::new()
                .create(true)
                .truncate(true)
                .read(true)
                .write(true)
                .open(self.dir.join(format!("data.{:03}", self.seg)))?;
        }
        let header = LocalHeader::new(*k, blob.len() as u32, self.off as usize);
        self.file.seek(SeekFrom::Start(self.off))?;
        self.file.write_all(&header.to_bytes())?;
        self.file.write_all(blob)?;
        let loc = Loc { seg: self.seg, off: self.off as u32, size: total as u32 };
        writeln!(self.journal, "{} {} {} {}", hex::encode(k), loc.seg, loc.off, loc.size)?;
        self.off += total;
        self.stored.insert(*k, loc);
        Ok(())
    }

    fn read_blob(&self, k: &Key) -> Result<Vec<u8>> {
        let loc = self.stored.get(k).ok_or_else(|| anyhow!("{} not stored", hex::encode(k)))?;
        let mut f = File::open(self.dir.join(format!("data.{:03}", loc.seg)))?;
        f.seek(SeekFrom::Start(u64::from(loc.off) + LOCAL_HEADER_SIZE as u64))?;
        let mut buf = vec![0u8; loc.size as usize - LOCAL_HEADER_SIZE];
        f.read_exact(&mut buf)?;
        Ok(buf)
    }
}

/// One CDN archive index, cached in Data/indices as Battle.net keeps it.
fn load_archive_index(cdn: &Cdn, root: &Path, hash: &str) -> Result<ArchiveIndex> {
    let path = root.join("Data").join("indices").join(format!("{hash}.index"));
    if let Ok(data) = std::fs::read(&path) {
        if let Ok(idx) = ArchiveIndex::parse(std::io::Cursor::new(&data)) {
            return Ok(idx);
        }
    }
    let data = cdn.get(&Cdn::url("data", hash, ".index"), None)?;
    let idx = ArchiveIndex::parse(std::io::Cursor::new(&data))
        .map_err(|e| anyhow!("index {hash}: {e}"))?;
    std::fs::create_dir_all(path.parent().unwrap())?;
    std::fs::write(&path, &data)?;
    Ok(idx)
}

/// Writes the .idx buckets for everything in the journal, with cascette's index manager.
fn write_idx(root: &Path, writer: &Writer) -> Result<()> {
    let data_dir = root.join("Data").join("data");
    for entry in std::fs::read_dir(&data_dir)? {
        let p = entry?.path();
        if p.extension().is_some_and(|e| e.eq_ignore_ascii_case("idx")) {
            std::fs::remove_file(p)?;
        }
    }
    let mut im = IndexManager::new(&data_dir);
    for (k, l) in &writer.stored {
        im.add_entry(&EncodingKey::from_bytes(*k), l.seg, l.off, l.size)
            .map_err(|e| anyhow!("index add: {e}"))?;
    }
    im.flush_all_updates().map_err(|e| anyhow!("index flush: {e}"))?;
    im.save_all().map_err(|e| anyhow!("index save: {e}"))?;
    println!("wrote {} index entries", im.entry_count());
    Ok(())
}

/// Rewrites only the .idx buckets from the journal, without downloading anything.
pub fn reindex(root: &Path) -> Result<()> {
    let writer = Writer::open(root)?;
    write_idx(root, &writer)
}

struct Range {
    archive: String,
    start: u64,
    end: u64,
    blobs: Vec<(Key, u64, u32)>, // key, archive offset, size
}

pub fn run(m: &Manifests) -> Result<()> {
    let root = &m.root;
    let t0 = Instant::now();

    // 1. What goes into local storage: the selected download entries, the manifests
    //    themselves, and the selected install files.
    let mut needed: HashSet<Key> = HashSet::new();
    let dl_groups = selection_masks(&m.download.tags, SELECT_TAGS)?;
    for (i, e) in m.download.entries.iter().enumerate() {
        if selected(&m.download.tags, &dl_groups, i) {
            needed.insert(*e.encoding_key.as_bytes());
        }
    }
    for k in ["encoding", "install", "download", "size"] {
        if let Ok(ek) = m.build.ekey(k) {
            needed.insert(key(ek)?);
        }
    }
    for (name, vals) in &m.build.0 {
        if name.starts_with("vfs-") && !name.ends_with("-size") && vals.len() == 2 {
            needed.insert(key(&vals[1])?);
        }
    }
    let root_ckey = &m.build.get("root")?[0];
    needed.insert(*m.ckey_to_ekey(root_ckey)?.as_bytes());

    let in_groups = selection_masks(&m.install.tags, SELECT_TAGS)?;
    let mut install_files = Vec::new();
    for (i, e) in m.install.entries.iter().enumerate() {
        if selected(&m.install.tags, &in_groups, i) {
            let ek = m.ckey_to_ekey(&e.content_key.to_hex())?;
            needed.insert(*ek.as_bytes());
            install_files.push((e.path.clone(), e.content_key.to_hex(), *ek.as_bytes()));
        }
    }
    println!("{} blobs to store, {} install files", needed.len(), install_files.len());

    // 2. Where each blob lives on the CDN.
    let archives = m.cdncfg.get("archives")?.to_vec();
    let pool = rayon::ThreadPoolBuilder::new().num_threads(THREADS).build()?;
    let done_idx = AtomicU64::new(0);
    let indexes: Vec<Result<ArchiveIndex>> = pool.install(|| {
        archives
            .par_iter()
            .map(|h| {
                let r = load_archive_index(&m.cdn, root, h);
                let n = done_idx.fetch_add(1, Ordering::Relaxed) + 1;
                if n % 100 == 0 {
                    println!("  archive indexes {n}/{}", archives.len());
                }
                r
            })
            .collect()
    });
    let mut located: HashMap<Key, (usize, u64, u32)> = HashMap::new();
    for (ai, idx) in indexes.into_iter().enumerate() {
        let idx = idx.with_context(|| format!("archive index {}", archives[ai]))?;
        for e in idx.entries {
            if e.encoding_key.len() < 16 {
                continue;
            }
            let k: Key = e.encoding_key[..16].try_into().unwrap();
            if needed.contains(&k) {
                located.insert(k, (ai, e.offset, e.size));
            }
        }
    }

    let writer = Mutex::new(Writer::open(root)?);
    let already = writer.lock().stored.len();
    let loose: Vec<Key> = needed
        .iter()
        .filter(|k| !located.contains_key(*k) && !writer.lock().stored.contains_key(*k))
        .copied()
        .collect();

    let mut per_archive: BTreeMap<usize, Vec<(Key, u64, u32)>> = BTreeMap::new();
    for (k, &(ai, off, size)) in &located {
        if !writer.lock().stored.contains_key(k) {
            per_archive.entry(ai).or_default().push((*k, off, size));
        }
    }
    let mut ranges = Vec::new();
    for (ai, mut blobs) in per_archive {
        blobs.sort_by_key(|b| b.1);
        let mut cur: Option<Range> = None;
        for b in blobs {
            let (s, e) = (b.1, b.1 + u64::from(b.2));
            match &mut cur {
                Some(r) if s <= r.end + MAX_GAP && e - r.start <= MAX_RANGE => {
                    r.end = r.end.max(e);
                    r.blobs.push(b);
                }
                _ => {
                    if let Some(r) = cur.take() {
                        ranges.push(r);
                    }
                    cur = Some(Range { archive: archives[ai].clone(), start: s, end: e, blobs: vec![b] });
                }
            }
        }
        ranges.extend(cur);
    }
    let fetch_bytes: u64 = ranges.iter().map(|r| r.end - r.start).sum();
    let blob_bytes: u64 = ranges.iter().flat_map(|r| &r.blobs).map(|b| u64::from(b.2)).sum();
    println!(
        "{already} already stored; {} archived blobs ({:.2} GB) in {} range requests ({:.2} GB on the wire); {} loose",
        ranges.iter().map(|r| r.blobs.len()).sum::<usize>(),
        blob_bytes as f64 / 1e9,
        ranges.len(),
        fetch_bytes as f64 / 1e9,
        loose.len()
    );

    // 3. Fetch and store, with a progress line every few seconds.
    let got = AtomicU64::new(0);
    let finished = AtomicBool::new(false);
    let failures = Mutex::new(Vec::<String>::new());
    std::thread::scope(|s| {
        s.spawn(|| {
            let start = Instant::now();
            while !finished.load(Ordering::Relaxed) {
                std::thread::sleep(Duration::from_secs(10));
                let g = got.load(Ordering::Relaxed);
                let secs = start.elapsed().as_secs_f64().max(1.0);
                println!(
                    "  {:.2}/{:.2} GB  {:.1} MB/s",
                    g as f64 / 1e9,
                    fetch_bytes as f64 / 1e9,
                    g as f64 / 1e6 / secs
                );
            }
        });
        pool.install(|| {
            ranges.par_iter().for_each(|r| {
                let res = (|| -> Result<()> {
                    let data = m.cdn.get(&Cdn::url("data", &r.archive, ""), Some((r.start, r.end)))?;
                    got.fetch_add(data.len() as u64, Ordering::Relaxed);
                    for (k, off, size) in &r.blobs {
                        let a = (off - r.start) as usize;
                        let blob = &data[a..a + *size as usize];
                        let ek = blte_ekey(blob)?;
                        if ek != hex::encode(k) {
                            bail!("{}: archive {} @{off} hashes to {ek}", hex::encode(k), r.archive);
                        }
                        writer.lock().store(k, blob)?;
                    }
                    Ok(())
                })();
                if let Err(e) = res {
                    failures.lock().push(format!("{} [{}..{}): {e:#}", r.archive, r.start, r.end));
                }
            });
            loose.par_iter().for_each(|k| {
                let hk = hex::encode(k);
                match m.cdn.data(&hk) {
                    Ok(blob) => {
                        got.fetch_add(blob.len() as u64, Ordering::Relaxed);
                        if let Err(e) = writer.lock().store(k, &blob) {
                            failures.lock().push(format!("{hk}: {e:#}"));
                        }
                    }
                    Err(e) => failures.lock().push(format!("{hk}: {e:#}")),
                }
            });
        });
        finished.store(true, Ordering::Relaxed);
    });
    let failures = failures.into_inner();
    if !failures.is_empty() {
        for f in failures.iter().take(20) {
            eprintln!("FAILED {f}");
        }
        bail!("{} fetches failed; rerun to retry them (stored blobs are kept)", failures.len());
    }
    let writer = writer.into_inner();
    writer.file.sync_all()?;
    println!("stored {} blobs in {:.0}s", writer.stored.len(), t0.elapsed().as_secs_f64());

    // 4. The local .idx buckets.
    write_idx(root, &writer)?;

    // 5. Loose install files, decoded and checked against their content keys.
    let flavor = root.join(FLAVOR_DIR);
    for (path, ckey, ek) in &install_files {
        let data = blte_decode(&writer.read_blob(ek)?).with_context(|| path.clone())?;
        crate::cdn::check_md5(&data, ckey).with_context(|| path.clone())?;
        let out = flavor.join(path.replace('\\', "/"));
        std::fs::create_dir_all(out.parent().unwrap())?;
        std::fs::write(&out, &data)?;
    }
    println!("wrote {} install files under {FLAVOR_DIR}", install_files.len());

    // 6. The files Battle.net leaves beside the data.
    std::fs::write(flavor.join(".flavor.info"), format!("Product Flavor!STRING:0\n{PRODUCT}\n"))?;
    let install_ekey = m.build.ekey("install")?;
    let im_size = m.build.get("install-size")?.get(1).cloned().unwrap_or_default();
    let build_info = format!(
        "Branch!STRING:0|Active!DEC:1|Build Key!HEX:16|CDN Key!HEX:16|Install Key!HEX:16|IM Size!DEC:4|CDN Path!STRING:0|CDN Hosts!STRING:0|CDN Servers!STRING:0|Tags!STRING:0|Armadillo!STRING:0|Last Activated!STRING:0|Version!STRING:0|KeyRing!HEX:16|Product!STRING:0\n\
         us|1|{BUILD_CONFIG}|{CDN_CONFIG}|{install_ekey}|{im_size}|{path}|{host}|https://{host}/?maxhosts=4|{BUILD_INFO_TAGS}|||{VERSION}||{PRODUCT}\n",
        path = crate::cdn::PATH,
        host = crate::cdn::HOST.trim_start_matches("https://"),
    );
    std::fs::write(root.join(".build.info"), build_info)?;
    println!("done in {:.0}s", t0.elapsed().as_secs_f64());
    Ok(())
}
