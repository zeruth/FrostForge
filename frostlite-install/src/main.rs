//! Installs World of Warcraft Classic 3.4.3.54261 (Windows, x86_64, enUS) from a
//! TACT CDN mirror into a Battle.net-shaped directory, using cascette-rs for every
//! format it touches.
//!
//! Blizzard's CDN has purged this build; casc.wago.tools still mirrors all of it.
//!
//!   frostlite-install plan    [<install-dir>]   fetch manifests, print tags and sizes
//!   frostlite-install install [<install-dir>]   download and lay out the client

mod cdn;
mod install;
mod manifests;
mod verify;

use anyhow::{Result, bail};
use std::path::PathBuf;

/// The pinned build. Hashes come from wago.tools' build history (product wow_classic).
pub const VERSION: &str = "3.4.3.54261";
pub const BUILD_CONFIG: &str = "c91609c69ed2ab39d44039390a1be969";
pub const CDN_CONFIG: &str = "a838aeb3cda2e027e9c96bd9953944b3";
pub const PRODUCT: &str = "wow_classic";
/// Battle.net installs a classic flavor into this subdirectory of the game root.
pub const FLAVOR_DIR: &str = "_classic_";
/// The tag string written to .build.info, in Battle.net's own form.
pub const BUILD_INFO_TAGS: &str = "Windows x86_64 US? enUS speech?:Windows x86_64 US? enUS text?";

/// The tag selection. Within a tag group a file must carry at least one of the
/// selected names; groups not named here do not filter.
pub const SELECT_TAGS: &[&str] = &["Windows", "x86_64", "enUS", "US"];

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let cmd = args.get(1).map(String::as_str).unwrap_or("");
    let root = args.get(2).map(PathBuf::from).unwrap_or_else(|| PathBuf::from("client"));

    match cmd {
        "plan" => {
            let m = manifests::Manifests::load(&root)?;
            m.print_plan();
            Ok(())
        }
        "install" => {
            let m = manifests::Manifests::load(&root)?;
            install::run(&m)
        }
        "reindex" => install::reindex(&root),
        "verify" => {
            let m = manifests::Manifests::load(&root)?;
            verify::run(&m)
        }
        _ => bail!("usage: frostlite-install plan|install|reindex|verify [<install-dir>]"),
    }
}
