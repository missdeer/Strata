import math
from pathlib import Path
import struct
import sys
from unittest.mock import patch

root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(root / "tools"))
from network_worker import ExpertFiles, NativeBackend

source = ExpertFiles("D:/Projects/Strata-data/packs/iq3_s",
                     "D:/Projects/Strata-data/models/IQ3_S/Qwen3.8-Flash-Next-GSQ-RCO-IQ3_S-00001-of-00002.gguf",
                     cache_mib=512)
backend = NativeBackend(root / "dist/cluster/host11/strata_network_backend.dll", workers=2)
try:
    experts = [2, 130, 258, 386]
    assignment = [0 if e in experts else 1 for e in range(512)]
    activation = b"".join(struct.pack("<f", math.sin(i * .13) * .2) for i in range(3 * 2560))
    entries = [(r, experts[r % 4]) for r in range(12)]
    reference = backend.compute(source, 0, 3, 4, entries, activation)
    messages = []
    pairs, size = source.preload(0, 2, assignment, progress=messages.append, headroom_bytes=0)
    # Run actual kernels with the file reader disabled: results must come from the preloaded private buffers.
    with patch.object(source, "blob", side_effect=AssertionError("expert file read after preload")):
        actual = backend.compute(source, 0, 3, 4, entries, activation)
    assert actual == reference
    print(messages[0]); print(messages[-1])
    print(f"Real IQ3_S preload: {pairs} expert pairs, {size} bytes; CPU outputs bitwise equal; no task-time file reads: PASS")
finally:
    backend.close()
    source.close()
