"""Build and run our Java adapter against the user's DevEco Studio installation."""

import asyncio
from contextlib import asynccontextmanager
import os
from pathlib import Path
import tempfile


def dependencies(studio):
    mirror = sorted((studio / "plugins/screen-mirroring/lib").glob("hosScrcpy-*.jar"))
    json_libraries = sorted((studio / "sdk/default/openharmony/js/build-tools/binary-tools").glob("fastjson2-[0-9]*.jar"))
    if len(mirror) != 1 or len(json_libraries) != 1:
        raise RuntimeError("This DevEco installation has an unsupported mirroring or JSON library layout.")
    jars = [mirror[0], studio / "lib/intellij.libraries.guava.jar",
            studio / "lib/intellij.libraries.gson.jar", json_libraries[0]]
    prefixes = ("grpc-", "protobuf-java-", "perfmark-api-", "proto-google-", "fastjson-", "fastjson2-extension-")
    jars += sorted(p for p in (studio / "plugins/harmony/lib").glob("*.jar") if p.name.startswith(prefixes))
    if not all(p.is_file() for p in jars):
        raise RuntimeError("Missing DevEco Studio runtime dependency.")
    return jars


async def compile_adapter(studio, classes):
    jars = dependencies(studio)
    source = Path(__file__).resolve().parent / "studio-bridge"
    classpath = os.pathsep.join(str(p) for p in [classes, *jars])
    proc = await asyncio.create_subprocess_exec(
        str(studio / "jbr/bin/javac.exe"), "-encoding", "UTF-8",
        "-classpath", classpath, "-d", str(classes), str(source / "HosLauncher.java"),
        str(source / "HarmonyBridge.java"),
    )
    if await proc.wait():
        raise RuntimeError("DevEco mirroring adapter failed to compile. Use a compatible installation or the UITest backend.")
    return classpath


@asynccontextmanager
async def session(studio, device, port, env):
    with tempfile.TemporaryDirectory(prefix="gs-studio-") as directory:
        classes = Path(directory)
        classpath = await compile_adapter(studio, classes)
        proc = await asyncio.create_subprocess_exec(
            str(studio / "jbr/bin/java.exe"), "-cp", str(classes), "HosLauncher",
            classpath, "HarmonyBridge", device.target, str(device.hdc), str(port),
            cwd=classes, env=env, stdout=asyncio.subprocess.PIPE,
        )
        async def ready():
            while line := await proc.stdout.readline():
                if line.startswith(b"BRIDGE_READY "):
                    return
            raise RuntimeError("DevEco mirroring adapter exited before it became ready.")

        async def drain():
            while await proc.stdout.read(65536):
                pass

        output = None
        started = False
        try:
            await asyncio.wait_for(ready(), 20)
            started = True
            output = asyncio.create_task(drain())
            yield
        finally:
            try:
                if proc.returncode is None and started:
                    _, writer = await asyncio.wait_for(asyncio.open_connection("127.0.0.1", port), 2)
                    writer.write(("GSH1 STOP " + env["GS_HARMONY_TOKEN"] + "\n").encode("ascii"))
                    await writer.drain()
                    writer.close()
                    await writer.wait_closed()
                    await asyncio.wait_for(proc.wait(), 6)
            except (OSError, asyncio.TimeoutError):
                pass
            finally:
                if proc.returncode is None:
                    proc.kill()
                    await proc.wait()
                if output:
                    await output
