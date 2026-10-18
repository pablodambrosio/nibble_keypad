# Nimble 4 - ESP32-C3 BLE HID Keyboard Firmware

A lightweight, robust Bluetooth Low Energy (BLE) HID 4-key macropad firmware for the **ESP32-C3** (RISC-V) SoC using the **ESP-IDF v6.0** framework and the **Apache NimBLE** Bluetooth stack.

---

## Features

- **Standard BLE HID Keyboard**: Transmits standard 8-byte HID keyboard reports (modifier byte, reserved byte, up to 6 simultaneous key roll-overs).
- **"Just Works" Frictionless Pairing**: Uses unauthenticated pairing with bonding enabled (`BLE_HS_IO_NO_INPUT_OUTPUT`) for zero-hassle pairing across Windows, macOS, Linux, iOS, and Android without requiring a numeric display.
- **Robust Debouncing & Atomic Reports**: Hardware inputs are sampled every 10 ms with a multi-read filter; concurrent key changes are bundled into a single atomic report to prevent BLE congestion and lost packets.
- **Automatic Stuck-Key Prevention**: Pending reports and key releases are tracked with retry flags to ensure key release packets are never lost, preventing runaway repeat keystrokes.
- **2-Stage Persistent Power Management**:
  - **Stage 1 (Connected Idle)**: After 10s (Energy Save) or 30s (Normal) of inactivity, enables BLE **Slave Latency** (skipping up to 30 connection intervals). The keyboard remains fully connected while powering down radio cycles.
  - **Stage 2 (Sleep Mode)**: After 5m (Energy Save) or 10m (Normal) of inactivity, terminates the connection cleanly and enters **Light Sleep** with low-level GPIO interrupt wakeup (< 1 ms latency on keypress).
  - **NVS Persistent Profiles**: Stored in non-volatile flash under `kb_cfg` and persists across reboots.

---

## Hardware & Pin Mapping

The firmware default pin configuration targets typical ESP32-C3 development boards (such as the Seeed Studio XIAO ESP32-C3):

| Key | GPIO Pin | Circuit Logic | Default Key Code | Output |
|---|---|---|---|---|
| **Key 1** | `GPIO 10` | Internal Pull-Up, Active-LOW | `0x04` | Key `a` |
| **Key 2** | `GPIO 8` | Internal Pull-Up, Active-LOW | `0x05` | Key `b` |
| **Key 3** | `GPIO 9` | Internal Pull-Up, Active-LOW | `0x06` | Key `c` |
| **Key 4** | `GPIO 20` | Internal Pull-Up, Active-LOW | `0x07` | Key `d` |

> [!WARNING]
> **ESP32-C3 Strapping Pin Caution**:
> - `GPIO 9` is the ESP32-C3 **BOOT strapping pin**. If Key 3 is held down while the board is powering on or being reset, the chip enters **ROM bootloader (download mode)** instead of booting firmware.
> - `GPIO 8` is also a boot strapping pin controlling boot log output.
> - If designing a dedicated custom PCB, it is strongly recommended to use **GPIO 0, 1, 2, 3** instead.

---

## Configurable Key Mappings & Modifier Combos

Every key can be independently configured with any combination of modifier keys (`Ctrl`, `Shift`, `Alt`, `GUI`) and a primary HID keycode. Key configurations are stored in **NVS flash** and persist across resets.

### Examples:
- **Simple letter**: `'h'` &rarr; `modifiers = KEY_MOD_NONE`, `keycode = 0x0B`
- **Copy shortcut**: `'Ctrl + c'` &rarr; `modifiers = KEY_MOD_LCTRL`, `keycode = 0x06`
- **Paste shortcut**: `'Ctrl + v'` &rarr; `modifiers = KEY_MOD_LCTRL`, `keycode = 0x19`
- **Triple combo**: `'Ctrl + Shift + v'` &rarr; `modifiers = KEY_MOD_LCTRL | KEY_MOD_LSHIFT`, `keycode = 0x19`
- **Pure modifier**: `'Shift'` &rarr; `modifiers = KEY_MOD_LSHIFT`, `keycode = 0x00`

### Programmatic Key Mapping API:
```c
// Retrieve configuration for Key 1..4 (index 0..3)
key_mapping_t mapping;
keyboard_get_key_mapping(0, &mapping);

// Remap Key 2 (index 1) to "Ctrl + Shift + V" and persist to NVS
keyboard_set_key_mapping(1, KEY_MOD_LCTRL | KEY_MOD_LSHIFT, 0x19);
```

---

## Power Management Architecture

The firmware implements two user-configurable power profiles:

```
Active Typing ────────► Stage 1: Connected Idle ────────► Stage 2: Sleep Mode
(Fast 10ms slot) 10s/30s (Stay Connected, Latency=30) 5m/10m (Radio Off, Wake on Key)
```

| Profile | Stage 1 (Connected Idle) | Stage 2 (Sleep Mode) | Target Use Case |
|---|---|---|---|
| **`POWER_PROFILE_ENERGY_SAVE`** *(Default)* | **10 seconds** | **5 minutes** (300 s) | Battery-powered portable use |
| **`POWER_PROFILE_NORMAL`** | **30 seconds** | **10 minutes** (600 s) | Desktop & continuous typing |

### Programmatic Profile API (GATT Ready)

```c
// Retrieve active profile (0 = NORMAL, 1 = ENERGY_SAVE)
power_profile_mode_t power_manager_get_profile(void);

// Switch profile and save to non-volatile flash (NVS)
esp_err_t power_manager_set_profile(power_profile_mode_t mode);
```

---

## Getting Started

### Prerequisites

- **ESP-IDF v6.0+** installed (e.g. at `C:\esp\v6.0\esp-idf`).
- Python 3.10+ with ESP-IDF virtual environment configured.
- USB-C cable connected to the ESP32-C3 board.

### Building & Flashing

1. **Activate the ESP-IDF Environment**:
   ```powershell
   . C:\esp\v6.0\esp-idf\export.ps1
   ```

2. **Build the Firmware**:
   ```powershell
   idf.py build
   ```

3. **Flash to Device and Open Serial Monitor**:
   ```powershell
   idf.py -p COM12 flash monitor
   ```
   *(Replace `COM12` with your actual serial port)*.

4. **Pairing**:
   - On your host computer, phone, or tablet, open **Bluetooth Settings**.
   - Search for **"Nimble 4"** and select **Pair**.
   - No passkey or PIN is required. The device will connect and register as an input keyboard.

---

## Project Structure

```
├── .vscode/               # VS Code project settings & clangd configuration
├── main/
│   ├── CMakeLists.txt     # Component registration and dependencies
│   ├── Kconfig.projbuild  # Component configuration menu
│   └── main.c            # Firmware implementation (BLE HID, key scanner, power manager)
├── docs/
│   ├── ARCHITECTURE.md    # Detailed software design & sequence diagrams
│   └── HARDWARE.md        # Electrical schematics, pin considerations & deep sleep notes
├── CMakeLists.txt         # Root build configuration
└── README.md              # Project documentation
```

---

## License

MIT License. See project headers for attribution.
