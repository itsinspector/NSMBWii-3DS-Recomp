"""Incremental Windows debug build of the runtime + generated code (MSVC).

    python tools/build_pc.py [--jobs N]

Output: build/pc/nsmbw_pc.exe. Run it with NSMBW_DATA pointing at a folder
holding game.iso and rel_image.bin (default: ./pcdata).
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

VSWHERE = r"C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
OUT = os.path.join("build", "pc")


def msvc_env():
    inst = subprocess.check_output([VSWHERE, "-latest", "-products", "*", "-property",
                                    "installationPath"], text=True).strip()
    bat = os.path.join(inst, "VC", "Auxiliary", "Build", "vcvars64.bat")
    out = subprocess.check_output(f'cmd /s /c ""{bat}" >nul && set"', text=True, shell=True)
    env = {}
    for line in out.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            env[k] = v
    return env


# generated code: small (/O1) but with intrinsics, so the byte swaps of every
# guest load/store inline instead of being calls
GEN_FLAGS = ["/w", "/Oi"]
FLAGS_VERSION = 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--trace", action="store_true", help="record guest function entries")
    args = ap.parse_args()
    env = msvc_env()
    os.makedirs(OUT, exist_ok=True)
    cl = shutil.which("cl", path=env.get("PATH", env.get("Path")))
    link = shutil.which("link", path=env.get("PATH", env.get("Path")))

    common = [cl, "/nologo", "/c", "/O1", "/Zi", "/Gy", "/std:c11", "/D_CRT_SECURE_NO_WARNINGS",
              "/Iruntime/include", "/Ibuild/gen", "/bigobj"]
    if args.trace:
        common.append("/DRT_TRACE_ENABLED")
    # switching trace on/off (or changing GEN_FLAGS) needs a full rebuild
    stamp = os.path.join(OUT, ("trace" if args.trace else "notrace") + f"-{FLAGS_VERSION}.stamp")
    if not os.path.exists(stamp):
        for s in glob.glob(os.path.join(OUT, "*.stamp")):
            os.remove(s)
        open(stamp, "w").close()
    gen_header_time = max(os.path.getmtime(p) for p in
                          ["runtime/include/ppc_context.h", "build/gen/ppc_funcs.h", stamp])
    rt_header_time = max(os.path.getmtime(p) for p in glob.glob("runtime/include/*.h") +
                         ["build/gen/ppc_funcs.h", stamp])
    jobs = []
    for src in sorted(glob.glob("runtime/src/*.c") + glob.glob("build/gen/*.c")):
        obj = os.path.join(OUT, os.path.basename(src)[:-2] + ".obj")
        header_time = gen_header_time if src.startswith("build") else rt_header_time
        if (os.path.exists(obj) and os.path.getmtime(obj) > os.path.getmtime(src)
                and os.path.getmtime(obj) > header_time):
            continue
        flags = ["/W3", "/O2", "/fp:fast"] if src.startswith("runtime") else GEN_FLAGS
        pdb = os.path.join(OUT, os.path.basename(src)[:-2] + ".pdb")
        jobs.append(common + flags + [src, "/Fo" + obj, "/Fd" + pdb])

    def run(cmd):
        r = subprocess.run(cmd, env=env, capture_output=True, text=True)
        return cmd[-3], r.returncode, r.stdout + r.stderr

    failed = False
    with ThreadPoolExecutor(args.jobs) as ex:
        for src, rc, out in ex.map(run, jobs):
            lines = [l for l in out.splitlines() if l.strip() and not l.strip().endswith(".c")]
            if lines:
                print("\n".join(lines))
            if rc:
                failed = True
                print(f"FAILED: {src}")
    if failed:
        sys.exit(1)
    # objects of sources that no longer exist (stale generated chunks)
    srcs = {os.path.basename(s)[:-2] for s in glob.glob("runtime/src/*.c") + glob.glob("build/gen/*.c")}
    for o in glob.glob(os.path.join(OUT, "*.obj")):
        if os.path.basename(o)[:-4] not in srcs:
            os.remove(o)
    objs = sorted(glob.glob(os.path.join(OUT, "*.obj")))
    exe = os.path.join(OUT, "nsmbw_pc.exe")
    r = subprocess.run([link, "/nologo", "/DEBUG", "/OPT:REF", "/STACK:8388608", "/OUT:" + exe] + objs,
                       env=env, capture_output=True, text=True)
    print(r.stdout + r.stderr)
    if r.returncode:
        sys.exit(1)
    print(f"built {exe} ({len(jobs)} files compiled)")


if __name__ == "__main__":
    main()
