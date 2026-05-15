mod build;

use clap::{Parser, Subcommand};
use std::path::PathBuf;
use std::process;

#[derive(Parser)]
#[command(name = "blyt", version = env!("BLYT_VERSION"), about = "Blyt cart development tool")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Compile and link a cart project into a .blyt cart file
    Build {
        /// Path to the cart project directory (default: current directory)
        #[arg(default_value = ".")]
        project_dir: PathBuf,

        /// Output path for the .blyt cart (default: <project-dir-name>.blyt)
        #[arg(short, long)]
        output: Option<PathBuf>,
    },
}

fn main() {
    let cli = Cli::parse();

    let result = match cli.command {
        Commands::Build {
            project_dir,
            output,
        } => build::run(&project_dir, output.as_deref()),
    };

    if let Err(e) = result {
        eprintln!("blyt: {e}");
        process::exit(1);
    }
}
