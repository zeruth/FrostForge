//! Reads the finished install back through cascette's own local-storage reader,
//! independently of the writer that produced it.

use crate::manifests::{Manifests, selected, selection_masks, work_dir};
use crate::{FLAVOR_DIR, SELECT_TAGS};
use anyhow::{Result, anyhow, bail};
use cascette_client_storage::Installation;
use cascette_client_storage::index::IndexManager;
use cascette_crypto::EncodingKey;
use std::io::BufRead;

pub fn run(m: &Manifests) -> Result<()> {
    let rt = tokio::runtime::Builder::new_current_thread().enable_all().build()?;
    rt.block_on(run_async(m))
}

async fn run_async(m: &Manifests) -> Result<()> {
    let root = &m.root;
    let data_dir = root.join("Data").join("data");

    // 1. Every journaled blob must be found in the .idx at the place it was written.
    let mut im = IndexManager::new(&data_dir);
    im.load_all().await.map_err(|e| anyhow!("idx load: {e}"))?;
    let journal = std::fs::File::open(work_dir(root).join("journal.txt"))?;
    let (mut ok, mut bad) = (0usize, 0usize);
    for line in std::io::BufReader::new(journal).lines() {
        let line = line?;
        let p: Vec<&str> = line.split_whitespace().collect();
        let ek = EncodingKey::from_hex(p[0])?;
        let want = (p[1].parse::<u16>()?, p[2].parse::<u32>()?, p[3].parse::<u32>()?);
        match im.lookup(&ek) {
            Some(e) if (e.archive_id(), e.archive_offset(), e.size) == want => ok += 1,
            other => {
                bad += 1;
                if bad <= 10 {
                    eprintln!("idx mismatch {}: want {want:?}, got {other:?}", p[0]);
                }
            }
        }
    }
    println!("idx: {ok} entries match, {bad} do not ({} entries loaded)", im.entry_count());

    // 2. Manifests and install files, read by encoding key and checked by content key.
    let inst = Installation::open(root.join("Data")).map_err(|e| anyhow!("{e}"))?;
    inst.initialize().await.map_err(|e| anyhow!("{e}"))?;
    let mut checks: Vec<(String, String, EncodingKey)> = Vec::new();
    for k in ["encoding", "install", "download", "size"] {
        let v = m.build.get(k)?;
        checks.push((k.to_string(), v[0].clone(), EncodingKey::from_hex(&v[1])?));
    }
    let root_ckey = m.build.get("root")?[0].clone();
    checks.push(("root".into(), root_ckey.clone(), m.ckey_to_ekey(&root_ckey)?));
    let groups = selection_masks(&m.install.tags, SELECT_TAGS)?;
    let mut files_ok = 0;
    for (i, e) in m.install.entries.iter().enumerate() {
        if selected(&m.install.tags, &groups, i) {
            let ck = e.content_key.to_hex();
            checks.push((e.path.clone(), ck.clone(), m.ckey_to_ekey(&ck)?));
            let on_disk = std::fs::read(root.join(FLAVOR_DIR).join(e.path.replace('\\', "/")))?;
            crate::cdn::check_md5(&on_disk, &ck)?;
            files_ok += 1;
        }
    }
    let mut failed = 0;
    for (name, ckey, ek) in &checks {
        match inst.read_file_by_encoding_key(ek).await {
            Ok(data) if format!("{:x}", md5::compute(&data)) == *ckey => {}
            Ok(_) => {
                failed += 1;
                eprintln!("content mismatch: {name}");
            }
            Err(e) => {
                failed += 1;
                eprintln!("read failed: {name}: {e}");
            }
        }
    }
    println!(
        "storage reads: {} of {} decode to their content keys; {files_ok} loose files on disk match",
        checks.len() - failed,
        checks.len()
    );
    if bad > 0 || failed > 0 {
        bail!("verification failed");
    }
    println!("verified");
    Ok(())
}
