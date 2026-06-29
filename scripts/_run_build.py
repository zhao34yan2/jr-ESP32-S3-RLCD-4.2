"""Run ESP-IDF build from MSys environment"""
import os, sys, subprocess

# Add tool paths using Windows format
tool_dirs = [
    r"E:\ESP\Espressif\.espressif\tools\cmake\3.30.2\bin",
    r"E:\ESP\Espressif\.espressif\tools\ninja\1.12.1",
    r"E:\ESP\Espressif\.espressif\tools\xtensa-esp-elf\esp-14.2.0_20251107\xtensa-esp-elf\bin",
]
os.environ["PATH"] = ";".join(tool_dirs) + ";" + os.environ.get("PATH", "")

# Set IDF vars
os.environ["IDF_PATH"] = r"E:\ESP\v5.5.2\esp-idf"
os.environ["IDF_TOOLS_PATH"] = r"E:\ESP\Espressif\.espressif"

# Unset MSYSTEM to bypass checks
os.environ.pop("MSYSTEM", None)

idf_py = os.path.join(os.environ["IDF_PATH"], "tools", "idf.py")
cmd = [sys.executable, idf_py] + sys.argv[1:]

print(f"Running: {' '.join(cmd)}")
sys.stdout.flush()

proc = subprocess.run(cmd)
sys.exit(proc.returncode)
