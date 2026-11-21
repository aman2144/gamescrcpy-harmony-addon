"""Launch GameScrcpy with a local bridge using public HDC and UITest commands."""

import argparse
import asyncio
from contextlib import asynccontextmanager
import os
from pathlib import Path
import re
import secrets
import shutil
import struct
import subprocess
import tempfile

BRIDGE_PORT = 52781
PHONE_AUDIO_PORT = 52782
HOST_AUDIO_PORT = 52783
AUDIO_BUNDLE = "org.example.gamescrcpy.audio"
MAX_FRAME_BYTES = 8 * 1024 * 1024


async def run(*args, timeout=15, stdin=None):
    result = await asyncio.to_thread(
        subprocess.run, [str(x) for x in args], input=stdin, capture_output=True,
        timeout=timeout, check=False,
    )
    if result.returncode:
        raise RuntimeError(f"{Path(args[0]).name} command failed ({result.returncode})")
    return result.stdout


class UsbDevice:
    def __init__(self, hdc, target):
        self.hdc = hdc
        self.target = target

    async def command(self, *args):
        reply = await run(self.hdc, "-t", self.target, *args)
        if b"[fail]" in reply.lower() or reply.lstrip().lower().startswith(b"error:"):
            raise RuntimeError("HDC command failed; check the authorized USB connection.")
        return reply

    @classmethod
    async def discover(cls, hdc):
        output = (await run(hdc, "list", "targets")).decode("utf-8", errors="replace")
        targets = [s.strip() for s in output.splitlines()
                   if re.fullmatch(r"[a-zA-Z0-9_.:-]+", s.strip())]
        if len(targets) != 1:
            raise RuntimeError("Connect exactly one unlocked phone and authorize USB debugging.")
        return cls(hdc, targets[0])

    async def snapshot(self, remote, local):
        await self.command("shell", "uitest", "screenCap", "-p", remote)
        await self.command("file", "recv", remote, local)
        image = local.read_bytes()
        if not image.startswith(b"\x89PNG\r\n\x1a\n"):
            raise ValueError("The device did not return a PNG screenshot.")
        return image


async def encode_picture(ffmpeg, image):
    return await run(
        ffmpeg, "-hide_banner", "-loglevel", "error", "-f", "image2pipe",
        "-i", "pipe:0", "-frames:v", "1", "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2",
        "-pix_fmt", "yuv420p", "-c:v", "libx264", "-preset", "ultrafast",
        "-tune", "zerolatency", "-f", "h264", "pipe:1", stdin=image,
    )


async def video(device, ffmpeg, writer):
    remote = "/data/local/tmp/gs-preview-" + secrets.token_hex(12) + ".png"
    try:
        with tempfile.TemporaryDirectory(prefix="gs-preview-") as temp:
            local = Path(temp) / "frame.png"
            while not writer.is_closing():
                image = await device.snapshot(remote, local)
                packet = await encode_picture(ffmpeg, image)
                if not 0 < len(packet) <= MAX_FRAME_BYTES:
                    raise ValueError("Unexpected preview frame size.")
                writer.write(struct.pack(">I", len(packet)) + packet)
                await writer.drain()
    finally:
        try:
            await device.command("shell", "rm", "-f", remote)
        except (OSError, RuntimeError, subprocess.TimeoutExpired):
            pass


async def control(device, reader, writer):
    start = None
    size = (0, 0)
    while line := await asyncio.wait_for(reader.readline(), 6):
        fields = line.decode("ascii").strip().split()
        ok = False
        if fields == ["P"]:
            ok = True
        elif fields == ["R"]:
            start, ok = None, True
        elif len(fields) == 3 and fields[0] == "D":
            w, h = map(int, fields[1:])
            ok = 0 <= w <= 4096 and 0 <= h <= 4096
            if ok:
                size = (w, h)
        elif len(fields) == 4 and fields[0] == "T":
            action, x, y = map(int, fields[1:])
            ok = action in (0, 1, 2, 3) and 0 <= x < size[0] and 0 <= y < size[1]
            if ok:
                if action == 0:
                    start = (x, y)
                elif action == 2 and start is not None:
                    if abs(x - start[0]) + abs(y - start[1]) < 6:
                        args = ["click", str(x), str(y)]
                    else:
                        args = ["swipe", *map(str, start), str(x), str(y)]
                    await device.command("shell", "uitest", "uiInput", *args)
                    start = None
                elif action == 3:
                    start = None
        elif fields in (["K", "3"], ["K", "4"]):
            key = "Home" if fields[1] == "3" else "Back"
            await device.command("shell", "uitest", "uiInput", "keyEvent", key)
            ok = True
        writer.write(b"OK\n" if ok else b"ER\n")
        await writer.drain()


async def audio(token, destination):
    reader, source = await asyncio.open_connection("127.0.0.1", HOST_AUDIO_PORT)
    try:
        source.write(b"GSA1 " + token.encode("ascii") + b"\n")
        await source.drain()
        greeting = await asyncio.wait_for(reader.readline(), 5)
        if greeting != b"GSA1 48000 2 S16LE\n":
            raise ValueError("Unrecognized playback capture format.")
        destination.write(greeting)
        while True:
            header = await reader.readexactly(4)
            count, = struct.unpack(">I", header)
            if not 4 <= count <= 192000 or count % 4:
                raise ValueError("Invalid PCM frame length.")
            destination.write(header + await reader.readexactly(count))
            await destination.drain()
    finally:
        source.close()
        await source.wait_closed()


@asynccontextmanager
async def audio_session(device, token, enabled):
    if not enabled:
        yield
        return
    local, remote = f"tcp:{HOST_AUDIO_PORT}", f"tcp:{PHONE_AUDIO_PORT}"
    mappings = await device.command("fport", "ls")
    if local.encode() in mappings:
        raise RuntimeError("The playback capture forwarding port is already in use.")
    await device.command("fport", local, remote)
    try:
        launched = await device.command("shell", "aa", "start", "-b", AUDIO_BUNDLE,
                                        "-a", "EntryAbility", "--ps", "token", token)
        if b"success" not in launched.lower():
            raise RuntimeError("Sign and install the audio companion before enabling audio.")
        try:
            yield
        finally:
            await device.command("shell", "aa", "force-stop", AUDIO_BUNDLE)
    finally:
        await device.command("fport", "rm", f"{local} {remote}")


async def main(options):
    gs = Path(options.game).resolve(strict=True)
    device = await UsbDevice.discover(shutil.which(options.hdc))
    token = secrets.token_hex(32)
    env = os.environ.copy()
    env.update(GS_HARMONY_PORT=str(BRIDGE_PORT), GS_HARMONY_TOKEN=token)
    env.pop("GS_HARMONY_AUDIO_PORT", None)
    if options.audio:
        env["GS_HARMONY_AUDIO_PORT"] = str(HOST_AUDIO_PORT)
    if options.studio:
        import studio_bridge
        async with audio_session(device, token, options.audio):
            async with studio_bridge.session(options.studio.resolve(strict=True), device, BRIDGE_PORT, env):
                process = await asyncio.create_subprocess_exec(str(gs), cwd=gs.parent, env=env)
                await process.wait()
        return
    active = set()
    claimed = set()

    async def serve(reader, writer):
        task = asyncio.current_task()
        active.add(task)
        channel = None
        try:
            line = await asyncio.wait_for(reader.readline(), 5)
            parts = line.decode("ascii").strip().split()
            if len(parts) != 3 or parts[0] != "GSH1" or not secrets.compare_digest(parts[2], token):
                return
            if parts[1] in claimed:
                return
            channel = parts[1]
            claimed.add(channel)
            if channel == "VIDEO":
                await video(device, options.ffmpeg, writer)
            elif channel == "CONTROL":
                await control(device, reader, writer)
            elif channel == "AUDIO" and options.audio:
                await audio(token, writer)
        except (OSError, ValueError, EOFError, RuntimeError, subprocess.TimeoutExpired):
            pass
        finally:
            if channel:
                claimed.discard(channel)
            active.discard(task)
            writer.close()
            try:
                await writer.wait_closed()
            except OSError:
                pass

    server = await asyncio.start_server(serve, "127.0.0.1", BRIDGE_PORT, limit=1024)
    try:
        async with server, audio_session(device, token, options.audio):
            process = await asyncio.create_subprocess_exec(str(gs), cwd=gs.parent, env=env)
            await process.wait()
    finally:
        pending = list(active)
        for task in pending:
            task.cancel()
        await asyncio.gather(*pending, return_exceptions=True)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Start a GameScrcpy USB preview session.")
    parser.add_argument("--game", required=True, help="Path to GameScrcpy.exe")
    parser.add_argument("--hdc", help="Path to the DevEco Studio HDC executable")
    parser.add_argument("--studio", type=Path, help="Use the DevEco Studio mirroring backend from this installation")
    parser.add_argument("--ffmpeg", default="ffmpeg", help="FFmpeg CLI with the libx264 encoder")
    parser.add_argument("--audio", action="store_true", help="Start the separately signed sound companion")
    args = parser.parse_args()
    if not args.hdc:
        args.hdc = str(args.studio / "sdk/default/openharmony/toolchains/hdc.exe") if args.studio else "hdc"
    for program in ([args.hdc] if args.studio else [args.hdc, args.ffmpeg]):
        if not shutil.which(program):
            parser.error(f"Executable not found: {program}")
    asyncio.run(main(args))
