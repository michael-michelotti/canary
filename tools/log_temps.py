#!/usr/bin/env python3
import csv, glob, time, sys, pathlib

def find_by_name(base_glob, target):
    for d in glob.glob(base_glob):
        name = pathlib.Path(d, "name")
        if name.exists() and name.read_text().strip() == target:
            return d
    sys.exit(f"could not find sensor named {target}")

def read_float(path):
    return float(pathlib.Path(path).read_text().strip())

cpu_path     = "/sys/class/thermal/thermal_zone0/temp"
tmp119_dir   = find_by_name("/sys/bus/iio/devices/iio:device*", "tmp119")
sht45_dir    = find_by_name("/sys/class/hwmon/hwmon*", "sht4x")

tmp119_raw   = f"{tmp119_dir}/in_temp_raw"
tmp119_scale = f"{tmp119_dir}/in_temp_scale"
sht45_input  = f"{sht45_dir}/temp1_input"

out = sys.argv[1] if len(sys.argv) > 1 else "temps.csv"
t0 = time.time()

with open(out, "w", newline="") as f:
    w = csv.writer(f)
    w.writerow(["t_sec", "t_cpu_c", "t_tmp119_c", "t_sht45_c"])
    while True:
        t_cpu    = read_float(cpu_path) / 1000.0
        t_tmp119 = read_float(tmp119_raw) * read_float(tmp119_scale) / 1000.0
        t_sht45  = read_float(sht45_input) / 1000.0
        w.writerow([f"{time.time()-t0:.1f}", f"{t_cpu:.3f}",
                    f"{t_tmp119:.3f}", f"{t_sht45:.3f}"])
        f.flush()
        time.sleep(2)
