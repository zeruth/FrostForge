//! Blocking CDN access with retries, against the mirror that still holds the build.

use anyhow::{Context, Result, anyhow, bail};
use std::time::Duration;

pub const HOST: &str = "https://casc.wago.tools";
pub const PATH: &str = "tpr/wow";

pub struct Cdn {
    http: reqwest::blocking::Client,
}

impl Cdn {
    pub fn new() -> Result<Self> {
        let http = reqwest::blocking::Client::builder()
            .timeout(Duration::from_secs(600))
            .connect_timeout(Duration::from_secs(30))
            .pool_max_idle_per_host(64)
            .build()?;
        Ok(Self { http })
    }

    pub fn url(kind: &str, hash: &str, suffix: &str) -> String {
        format!("{HOST}/{PATH}/{kind}/{}/{}/{hash}{suffix}", &hash[0..2], &hash[2..4])
    }

    /// GET a whole object, or `[start, end)` of it.
    pub fn get(&self, url: &str, range: Option<(u64, u64)>) -> Result<Vec<u8>> {
        let mut last = None;
        for attempt in 0..6 {
            if attempt > 0 {
                std::thread::sleep(Duration::from_secs(1 << attempt.min(5)));
            }
            let mut req = self.http.get(url);
            if let Some((s, e)) = range {
                req = req.header("Range", format!("bytes={}-{}", s, e - 1));
            }
            match req.send() {
                Ok(resp) => {
                    let status = resp.status();
                    if status.as_u16() == 404 {
                        bail!("404 {url}");
                    }
                    if !status.is_success() {
                        last = Some(anyhow!("HTTP {status} {url}"));
                        continue;
                    }
                    if range.is_some() && status.as_u16() != 206 {
                        bail!("server ignored the range request for {url}");
                    }
                    match resp.bytes() {
                        Ok(b) => {
                            if let Some((s, e)) = range {
                                if b.len() as u64 != e - s {
                                    last = Some(anyhow!("short read {} of {} {url}", b.len(), e - s));
                                    continue;
                                }
                            }
                            return Ok(b.to_vec());
                        }
                        Err(e) => last = Some(e.into()),
                    }
                }
                Err(e) => last = Some(e.into()),
            }
        }
        Err(last.unwrap_or_else(|| anyhow!("failed")).context(format!("GET {url}")))
    }

    /// A config file, checked against its name.
    pub fn config(&self, hash: &str) -> Result<Vec<u8>> {
        let data = self.get(&Self::url("config", hash, ""), None)?;
        check_md5(&data, hash).context("config")?;
        Ok(data)
    }

    /// A loose data object by encoding key, checked against its name.
    pub fn data(&self, ekey: &str) -> Result<Vec<u8>> {
        let data = self.get(&Self::url("data", ekey, ""), None)?;
        check_ekey(&data, ekey).context("data")?;
        Ok(data)
    }
}

/// The encoding key of a BLTE blob: MD5 of the header block when the blob is
/// chunked, of the whole blob when it is a single headerless chunk.
pub fn blte_ekey(data: &[u8]) -> Result<String> {
    if data.len() < 8 || &data[0..4] != b"BLTE" {
        bail!("not a BLTE blob");
    }
    let header_size = u32::from_be_bytes([data[4], data[5], data[6], data[7]]) as usize;
    let span = if header_size == 0 { data.len() } else { header_size };
    if span > data.len() {
        bail!("BLTE header runs past the blob");
    }
    Ok(format!("{:x}", md5::compute(&data[..span])))
}

pub fn check_ekey(data: &[u8], ekey: &str) -> Result<()> {
    let got = blte_ekey(data)?;
    if got != ekey {
        bail!("encoding key mismatch: expected {ekey}, got {got}");
    }
    Ok(())
}

pub fn check_md5(data: &[u8], hex_hash: &str) -> Result<()> {
    let got = format!("{:x}", md5::compute(data));
    if got != hex_hash {
        bail!("md5 mismatch: expected {hex_hash}, got {got}");
    }
    Ok(())
}
