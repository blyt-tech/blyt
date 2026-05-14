fn main() {
    let version = std::fs::read_to_string("../../build/version.txt")
        .expect("build/version.txt not found — run CMake configure first");
    println!("cargo:rustc-env=BLYT_VERSION={}", version.trim());
    println!("cargo:rerun-if-changed=../../build/version.txt");
}
