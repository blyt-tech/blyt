use std::env;
use std::process;

fn cmd_build(args: &[String]) {
    if args.is_empty() {
        eprintln!("usage: blyt build <source.c> [--output <out.blyt>]");
        eprintln!();
        eprintln!("  Compile and link a cart source file into a .blyt cart.");
        eprintln!();
        eprintln!("  Requires a configured RISC-V cross-compiler toolchain.");
        eprintln!("  Set BLYT_CLANG to the clang binary (default: clang).");
        eprintln!("  Example:");
        eprintln!("    BLYT_CLANG=clang-18 blyt build hello.c -o hello.blyt");
        eprintln!();
        eprintln!("  Note: full build support (SDK toolchain, .cart.info generation,");
        eprintln!("  linker script) is not yet implemented.");
        process::exit(1);
    }
    eprintln!("blyt build: not yet implemented");
    eprintln!("  source: {}", args[0]);
    process::exit(1);
}

fn main() {
    let args: Vec<String> = env::args().collect();

    if args.len() < 2 {
        println!("blyt {}", env!("BLYT_VERSION"));
        println!();
        println!("usage: blyt <command> [args]");
        println!();
        println!("commands:");
        println!("  build   compile and link a cart source file");
        return;
    }

    match args[1].as_str() {
        "build" => cmd_build(&args[2..]),
        cmd => {
            eprintln!("blyt: unknown command '{}'", cmd);
            process::exit(1);
        }
    }
}
