Import("env")

import hashlib
import subprocess
import sys
from pathlib import Path

project = Path(env.subst("$PROJECT_DIR"))
source_csv = project / "partitions" / "default_8MB.csv"
generated_dir = project / "src" / "generated"
generated_dir.mkdir(parents=True, exist_ok=True)
partition_bin = generated_dir / "default_8MB.partitions.bin"
header = generated_dir / "default_8MB_partitions.h"

platform = env.PioPlatform()
framework_dir = platform.get_package_dir("framework-arduinoespressif32")
if not framework_dir:
    raise RuntimeError("Could not locate framework-arduinoespressif32 package")

generator = Path(framework_dir) / "tools" / "gen_esp32part.py"
if not generator.exists():
    raise RuntimeError(f"Partition-table generator not found: {generator}")

subprocess.run(
    [sys.executable, str(generator), "-q", str(source_csv), str(partition_bin)],
    check=True,
)

data = partition_bin.read_bytes()
if len(data) > 0xC00:
    raise RuntimeError(f"Generated partition table is too large: {len(data):#x}")

# ESP-IDF partition-table verification consumes ESP_PARTITION_TABLE_MAX_LEN (0xC00).
data = data.ljust(0xC00, b"\xFF")

lines = [
    "// Generated from partitions/default_8MB.csv by gen_esp32part.py. Do not edit.",
    "#pragma once",
    "#include <stdint.h>",
    "alignas(4) static const uint8_t kDefault8MBPartitionTable[0xC00] = {",
]
for offset in range(0, len(data), 16):
    chunk = data[offset : offset + 16]
    lines.append("    " + ", ".join(f"0x{byte:02X}" for byte in chunk) + ",")
lines.append("};")
header.write_text("\n".join(lines) + "\n", encoding="utf-8")

print(f"Embedded default_8MB partition table: {len(data)} bytes; source sha256={hashlib.sha256(data).hexdigest()}")
