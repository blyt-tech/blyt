mod build;
mod export;
mod run;

use clap::{Args, Parser, Subcommand};
use std::path::{Path, PathBuf};
use std::process;

#[derive(Parser)]
#[command(name = "blyt", version = env!("BLYT_VERSION"), about = "Blyt cart development tool")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Compile a cart project or package it for a specific target
    Build(BuildArgs),

    /// Serve a .blyt cart in the browser via the WASM runtime
    Run {
        /// Path to the .blyt cart file to run
        cart: PathBuf,

        /// Enable debugging (DAP, GDB, frame stepping)
        #[arg(long)]
        debug: bool,
    },
}

#[derive(Args)]
struct BuildArgs {
    /// Build subcommand (e.g. wasm, all); omit to compile a cart from source
    #[command(subcommand)]
    sub: Option<BuildSubcommand>,

    /// Path to the cart project directory (default: current directory)
    project_dir: Option<PathBuf>,

    /// Output path (default: <project-dir-name>.blyt)
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Build with debug information (-g, -O0, path remapping for GDB/DAP)
    #[arg(long)]
    debug: bool,
}

#[derive(Subcommand)]
enum BuildSubcommand {
    /// Package a .blyt cart as a self-contained HTML page
    Wasm {
        /// Path to the .blyt cart file to package
        cart: PathBuf,

        /// Output path for the HTML file (default: <cart-name>.html)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// Compile a cart from source and package it as a self-contained HTML page
    All {
        /// Path to the cart project directory (default: current directory)
        project_dir: Option<PathBuf>,

        /// Output path for the .blyt cart (default: <project-dir-name>.blyt)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },

    /// Build a single library from src/lib/<name>/ in isolation
    Lib {
        /// Library name (subdirectory under src/lib/)
        name: String,

        /// Path to the cart project directory (default: current directory)
        project_dir: Option<PathBuf>,
    },
}

fn main() {
    let cli = Cli::parse();

    let result = match cli.command {
        Commands::Build(BuildArgs {
            sub: Some(BuildSubcommand::Wasm { cart, output }),
            ..
        }) => export::run(&cart, output.as_deref()).map_err(|e| e.to_string()),

        Commands::Build(BuildArgs {
            sub:
                Some(BuildSubcommand::All {
                    project_dir,
                    output,
                }),
            debug,
            ..
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::run(dir, output.as_deref(), debug)
                .map_err(|e| e.to_string())
                .and_then(|cart| export::run(&cart, None).map_err(|e| e.to_string()))
        }

        Commands::Build(BuildArgs {
            sub: None,
            project_dir,
            output,
            debug,
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::run(dir, output.as_deref(), debug)
                .map(|_| ())
                .map_err(|e| e.to_string())
        }

        Commands::Build(BuildArgs {
            sub: Some(BuildSubcommand::Lib { name, project_dir }),
            ..
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::build_single_lib(dir, &name)
                .map(|_| ())
                .map_err(|e| e.to_string())
        }

        Commands::Run { cart, debug } => run::run(&cart, debug).map_err(|e| e.to_string()),
    };

    if let Err(e) = result {
        eprintln!("blyt: {e}");
        process::exit(1);
    }
}
