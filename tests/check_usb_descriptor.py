"""Inspect the USB descriptors actually linked into the ESP32 ELF (pyelftools)."""
import struct
import sys
from elftools.elf.elffile import ELFFile

with open(sys.argv[1], "rb") as file:
    elf = ELFFile(file)
    def symbol(name):
        item = elf.get_section_by_name(".symtab").get_symbol_by_name(name)[0]
        section = elf.get_section(item["st_shndx"])
        offset = item["st_value"] - section["sh_addr"]
        return section.data()[offset:offset+item["st_size"]]

    dev = symbol("desc_device")
    config = symbol("desc_configuration")
    assert dev[0:2] == bytes([18, 1])
    assert struct.unpack_from("<H", config, 2)[0] == len(config)
    assert config[4] == 2, "Expected one control + one streaming interface"
    descriptors = []
    cursor = 0
    while cursor < len(config):
        size = config[cursor]
        assert size >= 2 and cursor + size <= len(config)
        descriptors.append(config[cursor:cursor+size])
        cursor += size
    interfaces = [d for d in descriptors if d[1] == 4]
    assert [(d[2],d[3],d[4]) for d in interfaces] == [(0,0,0),(1,0,0),(1,1,1)]
    assert all(d[5] == 1 and d[7] == 0x20 for d in interfaces), "Expected UAC2"
    endpoints = [d for d in descriptors if d[1] == 5]
    assert len(endpoints) == 1 and endpoints[0][2] == 0x82
    assert endpoints[0][3] & 3 == 1, "Expected isochronous audio"
    assert struct.unpack_from("<H", endpoints[0], 4)[0] == 98
    feature = next(d for d in descriptors if d[1:3] == bytes([0x24,6]))
    assert struct.unpack_from("<I", feature, 5)[0] == 15, "Master mute + volume"
    assert struct.unpack_from("<I", feature, 9)[0] == 0, "No separate mono channel control"
    formats = [d for d in descriptors if d[1:3] == bytes([0x24,2]) and len(d) == 6]
    assert len(formats) == 1 and formats[0][4:6] == bytes([2,16])
    print(f"PASS: {len(config)} descriptor bytes; UAC2, mono 16-bit, one IN endpoint, master-only controls.")
