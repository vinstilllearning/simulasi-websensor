#!/usr/bin/env python3
"""Embed the dashboard and compile the original ESP32/WROOM target."""
import argparse
from pathlib import Path
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]

if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cli", default="arduino-cli", help="Arduino CLI executable")
    parser.add_argument("--config", help="Optional Arduino CLI configuration file")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--build-path", type=Path, default=ROOT / "build/cache")
    parser.add_argument("--output", type=Path, default=ROOT / "build/binaries")
    args = parser.parse_args()
    subprocess.run([sys.executable, str(ROOT / "tools/embed_dashboard.py")], check=True)
    command = [args.cli, "compile", "--fqbn", "esp32:esp32:esp32", "--jobs", str(args.jobs),
               "--build-path", str(args.build_path.resolve()),
               "--output-dir", str(args.output.resolve())]
    if args.config:
        command += ["--config-file", str(Path(args.config).resolve())]
    command += [str(ROOT / "firmware/LampuHiasPintar")]
    result = subprocess.run(command, cwd=ROOT)
    sys.exit(result.returncode)
