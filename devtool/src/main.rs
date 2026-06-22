mod build;
mod cart_config_generated;
mod cart_info_generated;
mod cart_layouts_generated;
mod config;
mod engine;
mod export;
mod run;
mod setup;

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

    /// Serve a release .blyt cart in the browser via the WASM runtime
    Run {
        /// Path to the .blyt cart file to run
        cart: PathBuf,

        /// Trace channels for the served runtime (comma list of
        /// gdb,dap,lifecycle,api,frame — or "all"); falls back to BLYT_TRACE
        #[arg(long, value_name = "CHANNELS")]
        trace: Option<String>,
    },

    /// Serve a debug .dbg.blyt cart with the DAP/GDB debug runtime (ADR-0129)
    Debug {
        /// Path to the debug .dbg.blyt cart file to debug
        cart: PathBuf,

        /// Trace channels for the served runtime (comma list of
        /// gdb,dap,lifecycle,api,frame — or "all"); falls back to BLYT_TRACE
        #[arg(long, value_name = "CHANNELS")]
        trace: Option<String>,
    },

    /// Configure IDE and tooling integration for a cart project
    Setup {
        #[command(subcommand)]
        sub: SetupSubcommand,
    },
}

#[derive(Subcommand)]
enum SetupSubcommand {
    /// Generate .vscode/launch.json so F5 launches the Blyt DAP debugger
    Vscode {
        /// Path to the cart project directory (default: current directory)
        project_dir: Option<PathBuf>,

        /// Overwrite an existing .vscode/launch.json
        #[arg(long)]
        force: bool,
    },
}

#[derive(Args)]
struct BuildArgs {
    /// Build subcommand (e.g. wasm, all); omit to compile a cart from source
    #[command(subcommand)]
    sub: Option<BuildSubcommand>,

    /// Path to the cart project directory (default: current directory)
    project_dir: Option<PathBuf>,

    /// Output path (default: build/<id>.blyt, id from blyt.info.yaml)
    #[arg(short, long)]
    output: Option<PathBuf>,

    /// Build with debug information (-g, -O0, path remapping for GDB/DAP)
    #[arg(long)]
    debug: bool,

    /// Bypass incremental build state and rerun all tasks unconditionally
    #[arg(long)]
    force: bool,
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

        /// Output path for the .blyt cart (default: build/<id>.blyt, id from blyt.info.yaml)
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

    /// Build the dev ELF (build/.elf or build/.dbg.elf) for use with blytplay/blytdebug
    Code {
        /// Path to the cart project directory (default: current directory)
        project_dir: Option<PathBuf>,

        /// Build with debug information (produces build/.dbg.elf)
        #[arg(long)]
        debug: bool,

        /// Bypass incremental build state and rerun all tasks unconditionally
        #[arg(long)]
        force: bool,
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
            force,
            ..
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::run(dir, output.as_deref(), debug, force)
                .map_err(|e| e.to_string())
                .and_then(|cart| export::run(&cart, None).map_err(|e| e.to_string()))
        }

        Commands::Build(BuildArgs {
            sub: None,
            project_dir,
            output,
            debug,
            force,
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::run(dir, output.as_deref(), debug, force)
                .map(|_| ())
                .map_err(|e| e.to_string())
        }

        Commands::Build(BuildArgs {
            sub: Some(BuildSubcommand::Lib { name, project_dir }),
            force,
            debug,
            ..
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::build_single_lib(dir, &name, debug, force)
                .map(|_| ())
                .map_err(|e| e.to_string())
        }

        Commands::Build(BuildArgs {
            sub:
                Some(BuildSubcommand::Code {
                    project_dir,
                    debug,
                    force,
                }),
            ..
        }) => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            build::build_for_dev(dir, debug, force)
                .map(|_| ())
                .map_err(|e| e.to_string())
        }

        Commands::Run { cart, trace } => {
            run::run(&cart, trace.as_deref()).map_err(|e| e.to_string())
        }

        Commands::Debug { cart, trace } => {
            run::debug(&cart, trace.as_deref()).map_err(|e| e.to_string())
        }

        Commands::Setup {
            sub: SetupSubcommand::Vscode { project_dir, force },
        } => {
            let dir = project_dir.as_deref().unwrap_or(Path::new("."));
            setup::vscode(dir, force).map_err(|e| e.to_string())
        }
    };

    if let Err(e) = result {
        eprintln!("blyt: {e}");
        process::exit(1);
    }
}
