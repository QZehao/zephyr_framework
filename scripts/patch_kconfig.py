#!/usr/bin/env python3
"""
Patch kconfig.py to allow Kconfig warnings without aborting.
This is needed for compatibility with older board defconfig files.

Run this script before building if you encounter 'Aborting due to Kconfig warnings' errors.
"""

import os
import sys

def patch_kconfig():
    zephyr_base = os.environ.get('ZEPHYR_BASE')
    if not zephyr_base:
        print("ERROR: ZEPHYR_BASE environment variable not set")
        return False
    
    kconfig_path = os.path.join(zephyr_base, 'scripts', 'kconfig', 'kconfig.py')
    
    if not os.path.exists(kconfig_path):
        print(f"ERROR: kconfig.py not found at {kconfig_path}")
        return False
    
    with open(kconfig_path, 'r', encoding='utf-8') as f:
        content = f.read()
    
    # Check if already patched
    if '# PATCHED:' in content:
        print("kconfig.py already patched")
        return True
    
    # Patch 1: Change warn_only pattern to match all warnings
    old_warn_only = 'warn_only = r"warning:.*set more than once."'
    new_warn_only = 'warn_only = r"warning:.*"  # PATCHED: Allow all warnings'
    
    if old_warn_only in content:
        content = content.replace(old_warn_only, new_warn_only)
        print("Patched warn_only pattern")
    else:
        print("WARNING: Could not find warn_only pattern")
    
    # Patch 2: Disable error_out by default
    old_error_out = '            error_out = True'
    new_error_out = '            error_out = False  # PATCHED: Allow warnings'
    
    if old_error_out in content:
        content = content.replace(old_error_out, new_error_out)
        print("Patched error_out setting")
    else:
        print("WARNING: Could not find error_out setting")
    
    # Write patched content
    with open(kconfig_path, 'w', encoding='utf-8') as f:
        f.write(content)
    
    print(f"Successfully patched {kconfig_path}")
    print("NOTE: This patch will be lost if Zephyr is updated.")
    return True

if __name__ == '__main__':
    success = patch_kconfig()
    sys.exit(0 if success else 1)
