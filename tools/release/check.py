"""Reject release executables that require more than the glibc 2.28 ABI."""

import re
import subprocess
import sys


def readelf(*args):
    return subprocess.check_output(["readelf", "--wide", *args, sys.argv[1]], text=True)


def fail(message):
    sys.exit(f"release check: {message}")


program_headers = readelf("--program-headers")
interpreter = re.search(r"Requesting program interpreter: (.*?)\]", program_headers)
if interpreter is None or interpreter[1] != sys.argv[2]:
    fail("unexpected program interpreter")

dynamic = readelf("--dynamic")
if re.search(r"\((?:RPATH|RUNPATH)\)", dynamic):
    fail("RPATH or RUNPATH remains")

glibc_libraries = {
    "libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
    "libutil.so.1", "libresolv.so.2", "libanl.so.1",
    "ld-linux-x86-64.so.2", "ld-linux-aarch64.so.1",
}
needed = set(re.findall(r"\(NEEDED\).*?\[(.*?)\]", dynamic))
if "libc.so.6" not in needed or needed - glibc_libraries:
    fail(f"unexpected shared libraries: {sorted(needed)}")

versions = re.findall(r"\bName: (\S+)", readelf("--version-info"))
for version in versions:
    match = re.fullmatch(r"GLIBC_(\d+(?:\.\d+)*)", version)
    if match is None or tuple(map(int, match[1].split("."))) > (2, 28):
        fail(f"unsupported symbol version: {version}")
if not versions:
    fail("missing glibc symbol versions")
print(f"release check: glibc <= 2.28; shared libraries: {', '.join(sorted(needed))}")
