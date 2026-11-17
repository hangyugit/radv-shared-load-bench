#!/usr/bin/env python3
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import statistics
import subprocess

ROOT = Path(__file__).resolve().parent
RESULTS = ROOT / "results"
CASES = {
    "lat128": (0, 4, 4, 0),
    "tp128": (1, 4, 4, 0),
    "tp128-s32": (1, 4, 8, 0),
    "tp128-s128": (1, 4, 32, 0),
    "lat96": (0, 3, 4, 0),
    "lat64": (0, 2, 4, 0),
    "lat128-a8": (0, 4, 8, 2),
}


def launch(driver, case, iterations, groups, wave, samples, seed=1, dump=False, validate=False):
    env = os.environ.copy()
    for key in ("RADV_DEBUG", "ACO_DEBUG", "VK_ICD_FILENAMES", "VK_INSTANCE_LAYERS", "MESA_VK_TRACE", "MESA_VK_TRACE_PER_SUBMIT"):
        env.pop(key, None)
    env.update(VK_DRIVER_FILES=str(RESULTS / f"{driver}-icd.json"), MESA_SHADER_CACHE_DISABLE="true")
    if env.get("LDS_LIB_DIR"):
        env["LD_LIBRARY_PATH"] = env["LDS_LIB_DIR"] + os.pathsep + env.get("LD_LIBRARY_PATH", "")
    if dump:
        env.update(RADV_DEBUG="shaders,preoptir,shaderstats", ACO_DEBUG="validateir,validatera", LDS_WARMUP_MS="0")
    if validate:
        env.update(VK_INSTANCE_LAYERS="VK_LAYER_KHRONOS_validation", ACO_DEBUG="validateir,validatera", LDS_WARMUP_MS="0")
    mode, width, _, _ = CASES[case]
    command = [str(ROOT / "lds_vulkan"), str(ROOT / f"{case}.spv"), str(iterations), str(groups), str(wave), str(samples), str(mode), str(width), str(seed)]
    p = subprocess.run(command, env=env, text=True, capture_output=True, timeout=90)
    raw = "Command: " + shlex.join(command) + "\n" + p.stdout + p.stderr
    if p.returncode or "errors=0" not in p.stdout or "Validation Error" in p.stderr:
        raise RuntimeError(raw)
    fields = dict(re.findall(r"(\w+)=([^\s]+)", p.stdout.splitlines()[-1]))
    return fields, raw, p.stderr


def build():
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "vulkan"], text=True))
    subprocess.run(["g++", "-O2", "-std=c++17", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers", str(ROOT / "lds_vulkan.cpp"), *flags, "-o", str(ROOT / "lds_vulkan")], check=True)
    for name, (mode, width, stride, offset) in CASES.items():
        shader = ROOT / f"{name}.spv"
        subprocess.run(["glslangValidator", "-V", "--target-env", "vulkan1.3", f"-DMODE={mode}", f"-DWIDTH={width}", f"-DSTRIDE={stride}", f"-DOFFSET={offset}", str(ROOT / "shared_load.comp"), "-o", str(shader)], check=True)
        subprocess.run(["spirv-val", "--target-env", "vulkan1.3", str(shader)], check=True)


def dump():
    records = []
    for wave in (32, 64):
        for case in CASES:
            for driver in ("baseline", "patched"):
                fields, raw, stderr = launch(driver, case, 17, 2, wave, 1, dump=True, validate=True)
                name = f"{driver}-{case}-w{wave}"
                (RESULTS / f"{name}-ir.txt").write_text(raw)
                isa = stderr.split("disasm:\n")[-1]
                (RESULTS / f"{name}.isa").write_text(isa)
                binary = bytearray()
                previous = b""
                for line in isa.splitlines():
                    repeat = re.search(r"\(then repeated (\d+) times\)", line)
                    encoded = re.search(r"; ((?:[0-9a-f]{8})(?: [0-9a-f]{8})*)\s*$", line)
                    if repeat:
                        binary.extend(previous * int(repeat[1]))
                    elif encoded:
                        previous = b"".join(int(word, 16).to_bytes(4, "little") for word in encoded[1].split())
                        binary.extend(previous)
                if not binary:
                    raise RuntimeError(f"Missing ISA: {name}")
                record = dict(driver=driver, case=case, wave=wave, hash=fields["hash"],
                              spirv_sha256=hashlib.sha256((ROOT / f"{case}.spv").read_bytes()).hexdigest(),
                              isa_sha256=hashlib.sha256(binary).hexdigest(), code_bytes=len(binary),
                              loads={width: len(re.findall(rf"\bds_(?:load|read)_b{width}\b", isa)) for width in (32, 64, 96, 128)})
                records.append(record)
                print(name, record["loads"], flush=True)
    (RESULTS / "isa-comparison.json").write_text(json.dumps(records, indent=2) + "\n")
    for wave in (32, 64):
        for case in CASES:
            a, b = [r for r in records if r["case"] == case and r["wave"] == wave]
            assert a["hash"] == b["hash"]
            width = CASES[case][1] * 32
            if width in (96, 128) and CASES[case][3] == 0:
                assert a["loads"][width] == 0 and b["loads"][width] > 0, f"{width}-bit load not reproduced: {case}"
            else:
                assert a["isa_sha256"] == b["isa_sha256"], f"Control changed: {case}"
    print("PASS: equal outputs, b96/b128 enabled, and control ISA unchanged.")


def correctness():
    runs = []
    for case in CASES:
        for wave in (32, 64):
            for iterations, seed in ((1, 1), (17, 7), (257, 113)):
                for driver in ("baseline", "patched"):
                    fields, raw, _ = launch(driver, case, iterations, 7, wave, 3, seed, validate=True)
                    (RESULTS / f"check-{driver}-{case}-w{wave}-n{iterations}.txt").write_text(raw)
                    runs.append(dict(driver=driver, case=case, wave=wave, iterations=iterations, seed=seed, **{k: fields[k] for k in ("errors", "elements", "hash")}))
    (RESULTS / "correctness.json").write_text(json.dumps(runs, indent=2) + "\n")
    print(f"PASS: {len(runs)} Vulkan validation + CPU reference runs.")


def performance(bits=128):
    rows = []
    cases = (f"lat{bits}", f"tp{bits}", f"tp{bits}-s32", f"tp{bits}-s128", "lat96" if bits == 128 else "lat128", f"lat{bits}-a8")
    for wave in (32, 64):
        for case in cases:
            for repeat in range(5):
                for driver in (("baseline", "patched") if repeat % 2 == 0 else ("patched", "baseline")):
                    fields, raw, _ = launch(driver, case, 1024, 512, wave, 31)
                    (RESULTS / f"perf-{driver}-{case}-w{wave}-r{repeat}.txt").write_text(raw)
                    rows.append(dict(driver=driver, case=case, wave=wave, repeat=repeat, **fields))
                    (RESULTS / "performance.json").write_text(json.dumps(rows, indent=2) + "\n")
                    print(case, wave, repeat, driver, fields["median_us"], "us", flush=True)
    lines = ["| Case | Wave | Before µs (range) | After µs (range) | Median paired time reduction |", "|---|---:|---:|---:|---:|"]
    for wave in (32, 64):
        for case in cases:
            selected = [r for r in rows if r["case"] == case and r["wave"] == wave]
            times = {d: [float(r["median_us"]) for r in selected if r["driver"] == d] for d in ("baseline", "patched")}
            reductions = [100 * (1 - b / a) for a, b in zip(times["baseline"], times["patched"])]
            cells = [f"{statistics.median(t):.3f} ({min(t):.3f}–{max(t):.3f})" for t in times.values()]
            lines.append(f"| {case} | {wave} | {cells[0]} | {cells[1]} | {statistics.median(reductions):.2f}% |")
    (RESULTS / "performance.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("phase", choices=("build", "isa", "correctness", "performance", "all"))
    parser.add_argument("--bits", type=int, choices=(96, 128), default=128)
    parser.add_argument("--baseline", type=Path, help="RADV library with the original 64-bit shared-load limit.")
    parser.add_argument("--patched", type=Path, help="RADV library allowing both 96-bit and 128-bit shared loads.")
    args = parser.parse_args()
    RESULTS = Path(os.environ.get("LDS_RESULTS_DIR", ROOT / "results" / str(args.bits)))
    if args.bits == 96:
        CASES = {
            "lat96": (0, 3, 4, 0),
            "tp96": (1, 3, 4, 0),
            "tp96-s32": (1, 3, 8, 0),
            "tp96-s128": (1, 3, 32, 0),
            "lat128": (0, 4, 4, 0),
            "lat64": (0, 2, 4, 0),
            "lat96-a8": (0, 3, 8, 2),
        }
    RESULTS.mkdir(parents=True, exist_ok=True)
    if args.phase != "build":
        for driver, path in (("baseline", args.baseline), ("patched", args.patched)):
            if path is None or not path.expanduser().is_file():
                parser.error(f"--{driver} must name an existing libvulkan_radeon.so")
            manifest = {"file_format_version": "1.0.0", "ICD": {"library_path": str(path.expanduser().resolve()), "api_version": "1.4.0"}}
            (RESULTS / f"{driver}-icd.json").write_text(json.dumps(manifest, indent=2) + "\n")
    for phase, action in (("build", build), ("isa", dump), ("correctness", correctness), ("performance", performance)):
        if args.phase in (phase, "all"):
            if phase in ("build", "isa", "correctness"):
                action()
            else:
                action(args.bits)
