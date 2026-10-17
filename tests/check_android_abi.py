"""Check runtime-injected ABI slots, not just the mod factory export."""
import re
import subprocess
import sys

binary = sys.argv[1]
symbols = subprocess.check_output(['readelf', '--dyn-syms', '-W', binary], text=True)
required = {
    'create_kernel_mod': 'FUNC',
    'mod_logger_write': 'OBJECT',
    'patchlib_type_get_type': 'OBJECT',
    'patchlib_field_get_pointer': 'OBJECT',
    'patchlib_method_get_pointer': 'OBJECT',
    'patchlib_install_prepost_hook': 'OBJECT',
    'patchlib_array_at': 'OBJECT',
    'terraria_texture2d_create': 'OBJECT',
}
for name, kind in required.items():
    pattern = rf'^\s*\d+:\s+[0-9a-f]+\s+(\d+)\s+{kind}\s+GLOBAL\s+DEFAULT\s+(\d+)\s+{name}$'
    match = re.search(pattern, symbols, re.M)
    if not match:
        raise SystemExit(f'Loader ABI export missing or not writable slot: {name}')
    if kind == 'OBJECT' and int(match[1]) != 8:
        raise SystemExit(f'ARM64 function-pointer slot has wrong size: {name}')
header = subprocess.check_output(['readelf', '-h', binary], text=True)
assert 'AArch64' in header, 'Expected Android ARM64 binary'
print('ARM64 factory and all required injection slots are exported')
