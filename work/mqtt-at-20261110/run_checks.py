from pathlib import Path
import subprocess
import sys

folder = Path(__file__).resolve().parent
root = folder.parent.parent
compiler_folder = Path(r"C:\My_Software\Arm\Keil\ARM\ARMCLANG\bin")
compiler = compiler_folder / "armclang.exe"
linker = compiler_folder / "armlink.exe"
startup = folder / "startup.s"
includes = [folder / "include", root / "Treo/Service", root / "Treo/Common", root / "Treo/Driver"]

for source, output in [(folder / "checks.c", folder / "checks.o"), (startup, folder / "startup.o")]:
    command = [str(compiler), "--target=arm-arm-none-eabi", "-mcpu=cortex-m4", "-mthumb", "-mfloat-abi=soft"]
    if source.suffix == ".c":
        command += ["-D__MICROLIB", "-std=c99", "-O0", "-ffunction-sections", "-fdata-sections", "-fstack-usage"]
    command += ["-I" + str(path) for path in includes]
    command += ["-c", str(source), "-o", str(output)]
    subprocess.run(command, check=True)

subprocess.run([str(linker), "--cpu=Cortex-M4", "--library_type=microlib", "--entry=Reset_Handler", "--first=__Vectors", "--ro_base=0", "--rw_base=0x20000000", "--map", "--list=" + str(folder / "checks.map"), str(folder / "checks.o"), str(folder / "startup.o"), "-o", str(folder / "checks.axf")], check=True)
subprocess.run([sys.executable, str(root / "work/wifi-scan-parse-check/run_arm.py"), str(folder / "checks.axf")], check=True)
