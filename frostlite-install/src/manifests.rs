//! Configs and the three manifests that decide what gets installed.

use crate::cdn::Cdn;
use crate::{BUILD_CONFIG, CDN_CONFIG, SELECT_TAGS};
use anyhow::{Context, Result, anyhow, bail};
use cascette_crypto::{ContentKey, EncodingKey};
use cascette_formats::CascFormat;
use cascette_formats::blte::BlteFile;
use cascette_formats::download::DownloadManifest;
use cascette_formats::encoding::EncodingFile;
use cascette_formats::install::{InstallManifest, InstallTag};
use std::collections::BTreeMap;
use std::path::{Path, PathBuf};

/// `key = value [value...]` config text.
pub struct Config(pub BTreeMap<String, Vec<String>>);

impl Config {
    pub fn parse(data: &[u8]) -> Self {
        let mut map = BTreeMap::new();
        for line in String::from_utf8_lossy(data).lines() {
            let line = line.trim();
            if line.is_empty() || line.starts_with('#') {
                continue;
            }
            if let Some((k, v)) = line.split_once('=') {
                map.insert(
                    k.trim().to_string(),
                    v.split_whitespace().map(str::to_string).collect(),
                );
            }
        }
        Self(map)
    }

    pub fn get(&self, key: &str) -> Result<&[String]> {
        self.0
            .get(key)
            .map(Vec::as_slice)
            .ok_or_else(|| anyhow!("config has no '{key}'"))
    }

    /// The encoding key of a `ckey ekey` pair.
    pub fn ekey(&self, key: &str) -> Result<&str> {
        self.get(key)?
            .get(1)
            .map(String::as_str)
            .ok_or_else(|| anyhow!("'{key}' has no encoding key"))
    }
}

pub fn config_path(root: &Path, hash: &str) -> PathBuf {
    root.join("Data")
        .join("config")
        .join(&hash[0..2])
        .join(&hash[2..4])
        .join(hash)
}

/// Where downloads that are not part of the final layout are kept between runs.
pub fn work_dir(root: &Path) -> PathBuf {
    root.join(".frostlite")
}

/// A config file, cached in its Battle.net location under Data/config.
pub fn load_config(cdn: &Cdn, root: &Path, hash: &str) -> Result<Vec<u8>> {
    let path = config_path(root, hash);
    if let Ok(data) = std::fs::read(&path) {
        if crate::cdn::check_md5(&data, hash).is_ok() {
            return Ok(data);
        }
    }
    let data = cdn.config(hash)?;
    std::fs::create_dir_all(path.parent().unwrap())?;
    std::fs::write(&path, &data)?;
    Ok(data)
}

/// A loose BLTE object by encoding key, cached under the work directory.
pub fn load_blob(cdn: &Cdn, root: &Path, ekey: &str) -> Result<Vec<u8>> {
    let path = work_dir(root).join("blobs").join(ekey);
    if let Ok(data) = std::fs::read(&path) {
        if crate::cdn::check_ekey(&data, ekey).is_ok() {
            return Ok(data);
        }
    }
    let data = cdn.data(ekey)?;
    std::fs::create_dir_all(path.parent().unwrap())?;
    std::fs::write(&path, &data)?;
    Ok(data)
}

pub fn blte_decode(data: &[u8]) -> Result<Vec<u8>> {
    let blte = BlteFile::parse(data).map_err(|e| anyhow!("BLTE parse: {e}"))?;
    Ok(blte.decompress()?)
}

/// For each tag group, a file must be in one of the selected tags of that group.
/// Returns, per tag group present in the selection, the union bitmask.
pub fn selection_masks(tags: &[InstallTag], select: &[&str]) -> Result<Vec<(u16, Vec<usize>)>> {
    let mut groups: BTreeMap<u16, Vec<usize>> = BTreeMap::new();
    for name in select {
        let Some(i) = tags.iter().position(|t| t.name == *name) else {
            bail!("manifest has no tag '{name}'");
        };
        groups.entry(tags[i].tag_type as u16).or_default().push(i);
    }
    Ok(groups.into_iter().collect())
}

pub fn selected(tags: &[InstallTag], groups: &[(u16, Vec<usize>)], file: usize) -> bool {
    groups
        .iter()
        .all(|(_, idx)| idx.iter().any(|&t| tags[t].has_file(file)))
}

pub struct Manifests {
    pub root: PathBuf,
    pub cdn: Cdn,
    pub build_config: Vec<u8>,
    pub cdn_config: Vec<u8>,
    pub build: Config,
    pub cdncfg: Config,
    pub encoding_blte: Vec<u8>,
    pub encoding: EncodingFile,
    pub install: InstallManifest,
    pub download: DownloadManifest,
}

impl Manifests {
    pub fn load(root: &Path) -> Result<Self> {
        std::fs::create_dir_all(root)?;
        let cdn = Cdn::new()?;

        let build_config = load_config(&cdn, root, BUILD_CONFIG).context("build config")?;
        let cdn_config = load_config(&cdn, root, CDN_CONFIG).context("cdn config")?;
        let build = Config::parse(&build_config);
        let cdncfg = Config::parse(&cdn_config);

        println!("build: {}", build.get("build-name")?.join(" "));

        let encoding_blte = load_blob(&cdn, root, build.ekey("encoding")?).context("encoding")?;
        let encoding = EncodingFile::parse(&blte_decode(&encoding_blte)?)
            .map_err(|e| anyhow!("encoding parse: {e}"))?;
        println!(
            "encoding: {} ckeys, {} ekeys",
            encoding.ckey_count(),
            encoding.ekey_count()
        );

        let install_blte = load_blob(&cdn, root, build.ekey("install")?).context("install")?;
        let install = InstallManifest::parse(&blte_decode(&install_blte)?)
            .map_err(|e| anyhow!("install parse: {e}"))?;

        let download_blte = load_blob(&cdn, root, build.ekey("download")?).context("download")?;
        let download = DownloadManifest::parse(&blte_decode(&download_blte)?)
            .map_err(|e| anyhow!("download parse: {e}"))?;

        Ok(Self {
            root: root.to_path_buf(),
            cdn,
            build_config,
            cdn_config,
            build,
            cdncfg,
            encoding_blte,
            encoding,
            install,
            download,
        })
    }

    pub fn ckey_to_ekey(&self, ckey_hex: &str) -> Result<EncodingKey> {
        let ckey = ContentKey::from_hex(ckey_hex)?;
        self.encoding
            .find_encoding(&ckey)
            .ok_or_else(|| anyhow!("ckey {ckey_hex} not in encoding"))
    }

    pub fn print_plan(&self) {
        fn tags(label: &str, tags: &[InstallTag], count: usize) {
            println!("{label} tags:");
            for t in tags {
                println!(
                    "  {:<24} {:?} ({} files)",
                    t.name,
                    t.tag_type,
                    (0..count).filter(|&i| t.has_file(i)).count()
                );
            }
        }
        let ni = self.install.entries.len();
        let nd = self.download.entries.len();
        tags("install", &self.install.tags, ni);
        tags("download", &self.download.tags, nd);

        match selection_masks(&self.install.tags, SELECT_TAGS) {
            Ok(g) => {
                let files: Vec<_> = (0..ni).filter(|&i| selected(&self.install.tags, &g, i)).collect();
                let bytes: u64 = files.iter().map(|&i| u64::from(self.install.entries[i].file_size)).sum();
                println!("\ninstall selection {:?}: {} of {} files, {:.1} MB", SELECT_TAGS, files.len(), ni, bytes as f64 / 1e6);
                for &i in &files {
                    let e = &self.install.entries[i];
                    println!("  {:>12}  {}", e.file_size, e.path);
                }
            }
            Err(e) => println!("install selection: {e:#}"),
        }
        match selection_masks(&self.download.tags, SELECT_TAGS) {
            Ok(g) => {
                let files: Vec<_> = (0..nd).filter(|&i| selected(&self.download.tags, &g, i)).collect();
                let bytes: u64 = files.iter().map(|&i| self.download.entries[i].file_size.as_u64()).sum();
                let all: u64 = self.download.entries.iter().map(|e| e.file_size.as_u64()).sum();
                println!(
                    "download selection {:?}: {} of {} files, {:.2} GB of {:.2} GB",
                    SELECT_TAGS, files.len(), nd, bytes as f64 / 1e9, all as f64 / 1e9
                );
            }
            Err(e) => println!("download selection: {e:#}"),
        }
        println!("cdn archives: {}", self.cdncfg.get("archives").map(|a| a.len()).unwrap_or(0));
    }
}
