# Freestanding Zephyr Application Setup

This document explains how this project is configured as a **freestanding Zephyr application**.

## What is a Freestanding Application?

A freestanding Zephyr application is located **outside** of a Zephyr workspace. The application and Zephyr source code are in separate directories:

```
<home>/
├── zephyrproject/              # Zephyr workspace
│   ├── .west/
│   ├── zephyr/                 # Zephyr source (ZEPHYR_BASE)
│   ├── modules/
│   └── ...
└── zephyr_template/            # Application directory
    ├── CMakeLists.txt
    ├── prj.conf
    ├── src/
    └── ...
```

This is different from:
- **Repository applications**: Located inside the Zephyr repository
- **Workspace applications**: Located in a west workspace but outside the Zephyr repository

## Configuration

### Setting ZEPHYR_BASE

The build system needs to know where Zephyr is installed. This is done via the `ZEPHYR_BASE` environment variable.

**Method 1: Using zephyr_config.env (Recommended)**

This project includes a `zephyr_config.env` file that automatically sets the required environment variables:

```bash
ZEPHYR_BASE=D:/Code/1-github-code/zephyrproject/zephyr
ZEPHYR_SDK_INSTALL_DIR=D:/Code/1-github-code/zephyr-sdk-0.17.2
```

Edit this file to match your local installation paths.

**Method 2: Environment Variables**

Set the environment variables directly:

```bash
# Windows (PowerShell)
$env:ZEPHYR_BASE="D:/zephyrproject/zephyr"
$env:ZEPHYR_SDK_INSTALL_DIR="C:/zephyr-sdk"

# Linux/macOS
export ZEPHYR_BASE=/path/to/zephyr
export ZEPHYR_SDK_INSTALL_DIR=/path/to/zephyr-sdk
```

**Method 3: CMake Command Line**

Pass the variable directly to CMake:

```bash
west build -b <board> -DZEPHYR_BASE=/path/to/zephyr
```

## Building

### Standard Build

```bash
# Configure and build
west build -b <board> .

# Or using CMake directly
cmake -B build -GNinja -DBOARD=<board>
cmake --build build
```

### Build with Custom Configuration

```bash
# Use a specific configuration file
west build -b <board> -DCONF_FILE=prj.conf .

# Use additional configuration files
west build -b <board> -DEXTRA_CONF_FILE="extra.conf" .
```

### Clean Build

```bash
# Clean (preserves .config)
west build -t clean

# Pristine (removes everything)
west build -t pristine
```

## Custom Board Support

This project supports custom boards without modifying the Zephyr source tree.

### BOARD_ROOT Configuration

The `CMakeLists.txt` automatically sets `BOARD_ROOT` to include the `boards/` directory:

```cmake
list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/boards)
```

This allows you to add custom board definitions in the `boards/` directory.

### Board Directory Structure

```
boards/
└── vendor/
    └── board_name/
        ├── board_name_defconfig      # Default configuration
        ├── board_name.dts            # Device tree source
        ├── board_name.yaml           # Board documentation
        ├── board.cmake               # Board-specific CMake
        ├── Kconfig.defconfig         # Default Kconfig values
        ├── Kconfig.board             # Board Kconfig options
        └── board.h                   # Board header file
```

### Building with Custom Boards

```bash
# Build with custom board
west build -b vendor/board_name .

# The BOARD_ROOT is automatically configured
```

### Device Tree Overlays

This project includes several overlay options:

1. **Generic overlay**: `boards/overlay.dts`
   - Applied to all boards by default

2. **Board-specific overlay**: `boards/<board_name>.overlay`
   - Applied only when building for that specific board

3. **FILE_SUFFIX overlays**: `boards/<board_name>_<suffix>.overlay`
   - Used when building with `-DFILE_SUFFIX=<suffix>`

Example:
```bash
# Use variant overlay
west build -b nucleo_l4r5zi -DFILE_SUFFIX=mouse .
```

## SOC Support (Optional)

For custom SOC definitions, use `SOC_ROOT`:

```cmake
# In CMakeLists.txt, before find_package(Zephyr)
list(APPEND SOC_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/soc)
```

SOC directory structure:
```
soc/
└── vendor/
    └── soc_family/
        ├── Kconfig
        ├── Kconfig.soc
        └── Kconfig.defconfig
```

## Device Tree Support (Optional)

For custom device tree definitions, use `DTS_ROOT`:

```cmake
# In CMakeLists.txt, before find_package(Zephyr)
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/dts)
```

DTS directory structure:
```
dts/
├── include/
├── dts/common/
├── dts/arm/
├── dts/
└── dts/bindings/
```

## West Configuration

This project includes an optional `west.yml` manifest file. This is **optional** for freestanding applications.

### Option 1: Without west.yml (Recommended)

Simply set `ZEPHYR_BASE` and build:

```bash
# Set environment
source scripts/setup_env.sh  # or setup_env.bat

# Build
west build -b <board> .
```

### Option 2: With west.yml

Create a local west workspace:

```bash
# Initialize workspace
west init -l .

# Update repositories
west update

# Build
west build -b <board> .
```

## Troubleshooting

### ZEPHYR_BASE Not Found

```
FATAL_ERROR: ZEPHYR_BASE not set and zephyr_config.env not found.
```

**Solution**: 
1. Copy `zephyr_config.env.template` to `zephyr_config.env`
2. Edit the paths in `zephyr_config.env`
3. Or set `ZEPHYR_BASE` environment variable directly

### Board Not Found

```
error: Board 'vendor/board_name' not found
```

**Solution**:
1. Check that the board directory exists: `boards/vendor/board_name/`
2. Verify required files exist: `.defconfig`, `.dts`, `.yaml`
3. Ensure `BOARD_ROOT` is set correctly

### Build Directory Issues

```
error: Build directory is not configured for this board
```

**Solution**:
```bash
# Clean and rebuild
west build -t pristine
west build -b <board> .
```

## References

- [Zephyr Application Development Guide](https://docs.zephyrproject.org/latest/application/index.html)
- [Freestanding Applications](https://docs.zephyrproject.org/latest/application/index.html#zephyr-freestanding-app)
- [Custom Board Definitions](https://docs.zephyrproject.org/latest/application/index.html#custom-board-definition)
- [Important Build Variables](https://docs.zephyrproject.org/latest/application/index.html#important-build-vars)
