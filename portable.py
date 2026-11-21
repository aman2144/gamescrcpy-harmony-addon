"""Prepare the native extension on the user's PC and launch its USB bridge."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent
RUNTIME_DLLS = ("avcodec-63.dll", "avutil-61.dll", "swresample-7.dll", "vcruntime140.dll")


def choose_executable(name):
    dialog = (
        "Add-Type -AssemblyName System.Windows.Forms; "
        "$d=New-Object System.Windows.Forms.OpenFileDialog; "
        "$d.Filter='{0}|{0}'; $d.Title='Select {0}'; "
        "if ($d.ShowDialog() -eq 'OK') {{ "
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
        "[Console]::Write($d.FileName) }}"
    ).format(name)
    answer = subprocess.check_output(
        ["powershell.exe", "-NoProfile", "-STA", "-Command", dialog], encoding="utf-8"
    ).strip()
    if not answer:
        raise RuntimeError("已取消文件选择。")
    return Path(answer)


def prepare(game, output):
    folder = game.parent
    gs = folder / "gs.exe"
    if subprocess.check_output([gs, "--version"], encoding="utf-8").strip() != "gs 2.2.1":
        raise RuntimeError("请使用 GameScrcpy 2.2.1 Windows x64 的完整安装目录。")
    native = ROOT / "addon" / "native.dll"
    if not native.is_file():
        raise RuntimeError("缺少预编译模块。请解压 Releases 中的 Windows x64 ZIP。")
    with tempfile.TemporaryDirectory(prefix="harmony-addon-") as scratch:
        stage = Path(scratch) / "addon"
        shutil.copytree(native.parent, stage)
        for name in RUNTIME_DLLS:
            shutil.copy2(folder / name, stage / name)
        for name in ("FFmpeg.txt", "dav1d.txt", "Microsoft-Visual-C-runtime.txt"):
            shutil.copy2(folder / "licenses" / name, stage / "licenses" / name)
        package = Path(scratch) / "preview.gsext"
        subprocess.run([gs, "pack", stage, "-o", package], check=True)
        subprocess.run([gs, "validate", package], check=True)
        subprocess.run([gs, "ext", "test", package, "--hosted"], check=True)
        # Publish on the destination volume only when both checks passed.
        with tempfile.NamedTemporaryFile(dir=output.parent, delete=False) as tmp:
            temp_output = Path(tmp.name)
        try:
            shutil.copyfile(package, temp_output)
            os.replace(temp_output, output)
        finally:
            temp_output.unlink(missing_ok=True)
    print(f"已生成 {output}\n请在 GameScrcpy 扩展页面导入、启用，然后退出 GameScrcpy。")


def main():
    parser = argparse.ArgumentParser(description="准备或启动 GameScrcpy 鸿蒙 USB 扩展")
    actions = parser.add_mutually_exclusive_group()
    actions.add_argument("--prepare", action="store_true", help="根据本机的 GameScrcpy 生成扩展包")
    actions.add_argument("--mirror", action="store_true", help="使用 DevEco 连续投屏")
    actions.add_argument("--uitest", action="store_true", help="使用 HDC/FFmpeg 兼容模式")
    parser.add_argument("--game", type=Path, help="GameScrcpy.exe")
    parser.add_argument("--studio", type=Path, help="DevEco Studio 安装目录")
    parser.add_argument("--output", type=Path, default=ROOT / "harmony-preview.gsext")
    parser.add_argument("--hdc", type=Path)
    parser.add_argument("--ffmpeg", type=Path)
    parser.add_argument("--audio", action="store_true")
    args = parser.parse_args()
    if not (args.prepare or args.mirror or args.uitest):
        print("1. 首次使用：生成扩展包\n2. DevEco 连续投屏\n3. UITest 截屏兼容模式")
        choice = input("请选择 1、2 或 3：").strip()
        if choice not in ("1", "2", "3"):
            return
        args.prepare, args.mirror, args.uitest = choice == "1", choice == "2", choice == "3"
    game = (args.game or choose_executable("GameScrcpy.exe")).resolve(strict=True)
    if game.name.casefold() != "gamescrcpy.exe":
        raise RuntimeError("请选择 GameScrcpy.exe。")
    if args.prepare:
        prepare(game, args.output.resolve())
        return
    command = [sys.executable, str(ROOT / "bridge.py"), "--game", str(game)]
    if args.mirror:
        default = os.environ.get("HARMONY_STUDIO") or str(Path(os.environ["ProgramFiles"]) / "Huawei/DevEco Studio")
        studio = args.studio or Path(default)
        if not (studio / "bin/devecostudio64.exe").is_file() and not args.studio:
            studio = choose_executable("devecostudio64.exe").parent.parent
        command.extend(["--studio", str(studio)])
    for name in ("hdc", "ffmpeg"):
        if getattr(args, name):
            command.extend(["--" + name, str(getattr(args, name))])
    if args.audio:
        command.append("--audio")
    subprocess.run(command, check=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        sys.exit(str(error))
