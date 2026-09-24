"""Short Unreal console entry point for setup_gamepad_input.py."""

from pathlib import Path


script_path = Path(__file__).with_name("setup_gamepad_input.py")
exec(compile(script_path.read_text(encoding="utf-8"), str(script_path), "exec"))
