#!/usr/bin/env python3
"""Extract exact ELF objects/layouts, without guessing MCU RAM address regions.

Use Zephyr's rom_report/ram_report for total ROM/RAM, including executable RAM.
Never sum all writable sections and call that total physical RAM on ESP32.
"""
import csv
from pathlib import Path
from elftools.elf.elffile import ELFFile


def type_size(die, seen=None):
    if die is None:
        return None
    seen = set() if seen is None else seen
    if die.offset in seen:
        return None
    seen = seen | {die.offset}
    attr = die.attributes.get("DW_AT_byte_size")
    if attr is not None:
        return int(attr.value)
    if die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type"):
        return type_size(die.get_DIE_from_attribute("DW_AT_type"), seen)
    if die.tag == "DW_TAG_array_type":
        element = type_size(die.get_DIE_from_attribute("DW_AT_type"), seen)
        count = 1
        if element is None:
            return None
        dimensions = list(die.iter_children())
        if not dimensions:
            return None
        for subrange in dimensions:
            if subrange.tag != "DW_TAG_subrange_type":
                continue
            attrs = subrange.attributes
            if "DW_AT_count" in attrs:
                n = int(attrs["DW_AT_count"].value)
            elif "DW_AT_upper_bound" in attrs:
                low = int(attrs["DW_AT_lower_bound"].value) if "DW_AT_lower_bound" in attrs else 0
                n = int(attrs["DW_AT_upper_bound"].value) - low + 1
            else:
                return None
            if n <= 0:
                return None
            count *= n
        return element * count
    return None


def inspect_elf(path, csv_path):
    with Path(path).open("rb") as f:
        elf = ELFFile(f)
        symtab = elf.get_section_by_name(".symtab")
        if symtab is None:
            raise ValueError("ELF has no symbol table; supply the unstripped zephyr.elf")
        symbols = []
        for symbol in symtab.iter_symbols():
            if symbol.name.startswith("zfm_x0_"):
                section_id = symbol["st_shndx"]
                section = elf.get_section(section_id).name if isinstance(section_id, int) else str(section_id)
                symbols.append({"name": symbol.name, "address": hex(symbol["st_value"]),
                                "bytes": symbol["st_size"], "type": symbol["st_info"]["type"],
                                "section": section})
        layouts = []
        has_dwarf = elf.has_dwarf_info(strict=True)
        if has_dwarf:
            dwarf = elf.get_dwarf_info()
            for cu in dwarf.iter_CUs():
                for die in cu.iter_DIEs():
                    name = die.attributes.get("DW_AT_name")
                    if die.tag != "DW_TAG_structure_type" or name is None or name.value != b"zfm_x0_data":
                        continue
                    size = type_size(die)
                    if size is None:
                        continue
                    members = []
                    for member in die.iter_children():
                        if member.tag != "DW_TAG_member":
                            continue
                        mn = member.attributes.get("DW_AT_name")
                        loc = member.attributes.get("DW_AT_data_member_location")
                        offset = int(loc.value) if loc is not None and isinstance(loc.value, int) else None
                        members.append({"name": mn.value.decode(errors="replace") if mn else "<anonymous>",
                                        "offset_bytes": offset,
                                        "storage_bytes": type_size(member.get_DIE_from_attribute("DW_AT_type"))})
                    layout = {"struct_bytes": size, "members": members}
                    if layout not in layouts:
                        layouts.append(layout)
        data_objects = [s for s in symbols if s["name"].startswith("zfm_x0_data_") and s["type"] == "STT_OBJECT"]
        with Path(csv_path).open("x", newline="") as out:
            w = csv.DictWriter(out, fieldnames=("name", "address", "bytes", "type", "section"))
            w.writeheader(); w.writerows(symbols)
        return {"elf": str(path), "machine": elf["e_machine"], "class_bits": elf.elfclass,
                "has_dwarf_info": has_dwarf,
                "driver_data_objects": data_objects, "zfm_x0_data_layouts": layouts,
                "accounting_note": "Each zfm_x0_data object includes its inline TX/RX packet storage and synchronization state. ZFM has no private worker stack. "
                "DWARF member sizes describe reserved storage, not dynamic allocation or observed stack use. "
                "Use native Zephyr reports for total ROM/RAM; symbol sums omit alignment and shared dependencies."}
