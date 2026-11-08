// build.rs - the Windows resources of tfa-setup.exe: its version information (the Details tab of
// the file's Properties: who made it, what it is, its version, its licence) and its application
// manifest (runs as the invoking user, the Windows versions it supports, DPI and long-path aware).
// The version is the repository's VERSION file (one level up: the game's own version). Nothing here
// downloads anything; the resource compiler (rc.exe) comes with the Visual Studio C++ tools.
use std::path::PathBuf;

fn main() {
    let version_file = PathBuf::from("..").join("VERSION");
    println!("cargo:rerun-if-changed={}", version_file.display());
    println!("cargo:rerun-if-changed=tfa-setup.rc.in");
    println!("cargo:rerun-if-changed=tfa-setup.manifest");
    println!("cargo:rerun-if-env-changed=TFA_REPO_URL");

    // "0.2.3" -> 0,2,3,0 (a version that does not read as three numbers counts as 0.0.0)
    let text = std::fs::read_to_string(&version_file).unwrap_or_default();
    let parts: Vec<u16> = text.trim().split('.').map(|p| p.parse().unwrap_or(0)).collect();
    let (major, minor, patch) = if parts.len() == 3 { (parts[0], parts[1], parts[2]) } else { (0, 0, 0) };
    let dotted = format!("{major}.{minor}.{patch}");

    // (the same address main.rs takes: TFA_REPO_URL at build time, else the public repository)
    let repo = std::env::var("TFA_REPO_URL").unwrap_or_else(|_| "https://github.com/kayliox/transformers-armada-recomp".into());
    let manifest = std::fs::canonicalize("tfa-setup.manifest").expect("tfa-setup.manifest");
    let rc = std::fs::read_to_string("tfa-setup.rc.in")
        .expect("tfa-setup.rc.in")
        .replace("@VERSION_COMMAS@", &format!("{major},{minor},{patch},0"))
        .replace("@VERSION@", &dotted)
        .replace("@REPO@", &repo)
        // (rc.exe reads the manifest at build time and embeds its contents, not its path)
        .replace("@MANIFEST@", &manifest.to_string_lossy().replace('\\', "/").trim_start_matches("//?/").to_string());
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("tfa-setup.rc");
    std::fs::write(&out, rc).unwrap();
    embed_resource::compile(&out, embed_resource::NONE).manifest_required().unwrap();
}
