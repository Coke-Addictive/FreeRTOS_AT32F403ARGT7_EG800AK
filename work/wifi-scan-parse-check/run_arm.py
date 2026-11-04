from pathlib import Path
import struct
import sys

sys.path.insert(0, str(Path(__file__).parent / "python-deps"))

from elftools.elf.elffile import ELFFile
from unicorn import Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_MODE_MCLASS, UC_HOOK_INTR
from unicorn.arm_const import UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP, UC_ARM_REG_PC

image_path = Path(sys.argv[1])
machine = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
machine.mem_map(0, 0x200000)
machine.mem_map(0x20000000, 0x200000)

with image_path.open("rb") as image:
    elf = ELFFile(image)
    for segment in elf.iter_segments():
        if segment["p_type"] == "PT_LOAD" and segment["p_filesz"]:
            machine.mem_write(segment["p_paddr"], segment.data())
            if segment["p_vaddr"] != segment["p_paddr"]:
                machine.mem_write(segment["p_vaddr"], segment.data())

exit_status = None
semihost_calls = 0


def semihost(machine, number, _):
    global exit_status, semihost_calls
    semihost_calls += 1
    if semihost_calls > 100:
        raise RuntimeError("Exceeded the diagnostic semihost call limit")
    operation = machine.reg_read(UC_ARM_REG_R0)
    address = machine.reg_read(UC_ARM_REG_R1)
    if operation == 4:
        output = bytearray()
        while True:
            value = machine.mem_read(address + len(output), 1)[0]
            if value == 0:
                break
            output.append(value)
        print(output.decode("utf-8"), end="", flush=True)
    elif operation == 0x20:
        _, exit_status = struct.unpack("<II", machine.mem_read(address, 8))
        machine.emu_stop()
    else:
        raise RuntimeError(f"Unhandled interrupt {number}, semihost operation {operation:#x}")
    machine.reg_write(UC_ARM_REG_PC, (machine.reg_read(UC_ARM_REG_PC) + 2) | 1)


machine.hook_add(UC_HOOK_INTR, semihost)
stack_pointer, reset_handler = struct.unpack("<II", machine.mem_read(0, 8))
machine.reg_write(UC_ARM_REG_SP, stack_pointer)
machine.emu_start(reset_handler | 1, 0, timeout=10000000, count=30000000)
if exit_status is None:
    raise RuntimeError(f"Simulation did not exit, PC={machine.reg_read(UC_ARM_REG_PC):#x}")
print(f"Arm MicroLIB test exit code: {exit_status}")
sys.exit(exit_status)
