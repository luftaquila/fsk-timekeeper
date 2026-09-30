"""Regression: the nRF52 bootloader ignores partial UF2 payloads."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

converter = Path(__file__).resolve().parents[1] / 'tools' / 'bin2uf2.py'
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    for length in (1, 255, 256, 257, 4097):
        original = bytes(i % 251 for i in range(length))
        (root / 'app.bin').write_bytes(original)
        subprocess.run([sys.executable, str(converter), '--base', '0x26000',
                        '--family', '0xADA52840', str(root / 'app.bin'), str(root / 'app.uf2')],
                       check=True, capture_output=True)
        data = (root / 'app.uf2').read_bytes()
        accepted = []
        reconstructed = bytearray()
        for offset in range(0, len(data), 512):
            block = data[offset:offset + 512]
            magic0, magic1, flags, addr, size, index, total, family = struct.unpack_from('<8I', block)
            assert (magic0, magic1, struct.unpack_from('<I', block, 508)[0]) == (0x0A324655, 0x9E5D5157, 0x0AB16F30)
            assert flags & 0x2000 and not flags & 1 and family == 0xADA52840
            # Same acceptance conditions as the installed nRF52 bootloader.
            assert size == 256 and addr & 0xFF == 0
            assert addr == 0x26000 + index * 256
            assert total == len(data) // 512
            accepted.append(index)
            reconstructed.extend(block[32:32 + size])
        assert accepted == list(range((length + 255) // 256))
        assert reconstructed[:length] == original
        assert all(value == 0xFF for value in reconstructed[length:])
print('PASS uf2: short and page-crossing images complete with intact payloads')
