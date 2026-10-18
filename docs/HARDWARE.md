# Hardware Specification & Electrical Design

This document details the hardware characteristics, pin allocations, power domain architecture, and electrical considerations for the **Nimble 4** keyboard.

---

## 1. Pin Assignment & Connection Diagram

The 4 mechanical keys are connected directly between the ESP32-C3 GPIO pins and ground (GND):

```
       ESP32-C3
  +------------------+
  |                  |
  |          GPIO 10 +-------[ Key 1 ]-------+
  |                  |                       |
  |          GPIO  8 +-------[ Key 2 ]-------+
  |                  |                       |
  |          GPIO  9 +-------[ Key 3 ]-------+
  |                  |                       |
  |          GPIO 20 +-------[ Key 4 ]-------+
  |                  |                       |
  |              GND +-----------------------+
  +------------------+
```

### Pin Characteristics:
- **Mode**: Digital Input (`GPIO_MODE_INPUT`)
- **Bias**: Internal pull-up enabled (`GPIO_PULLUP_ENABLE`)
- **Active State**: LOW (`0` when switch is closed/pressed; `1` when released)
- **External Components**: None required; switch debounce is handled in firmware.

---

## 2. ESP32-C3 Strapping Pins (Critical Caution)

The ESP32-C3 uses specific pins during chip reset to determine the boot mode:

| GPIO Pin | Function in Firmware | ESP32-C3 Strapping Function | Hardware Risk & Notes |
|---|---|---|---|
| **`GPIO 9`** | Key 3 | **BOOT Pin** (High = Flash boot, Low = Download mode) | **High Risk**: If Key 3 is held down while plugging in USB or powering on the board, the ESP32-C3 enters the ROM serial bootloader instead of running firmware. |
| **`GPIO 8`** | Key 2 | Boot Log & Strapping | Low Risk: Controls ROM boot message printing. |
| **`GPIO 10`** | Key 1 | General GPIO | Safe. |
| **`GPIO 20`** | Key 4 | General GPIO / USB-Serial-JTAG | Safe. |

---

## 3. Power Domain Architecture: Deep Sleep vs. Light Sleep

The ESP32-C3 silicon consists of two power domains relevant to sleep:
1. **`VDD3P3_CPU` (HP Peripheral Domain)**: Powers the CPU, Wi-Fi/Bluetooth RF transceivers, and digital GPIOs 6 through 21. This domain is **completely unpowered in Deep Sleep**.
2. **`VDD3P3_RTC` (LP Core Domain)**: Powers the ultra-low-power controller, RTC timer, and only **GPIO 0 through GPIO 5**.

### Silicon Sleep Capability Matrix:

| Feature | Light Sleep (`esp_light_sleep_start`) | Deep Sleep (`esp_deep_sleep_start`) |
|---|---|---|
| **Supported Wakeup Pins** | **ALL GPIOs (0 to 21)** | **ONLY GPIO 0, 1, 2, 3, 4, 5** (`BIT0..BIT5`) |
| **Current Consumption** | **~130 µA** | **~5 µA** |
| **RAM Retention** | **Preserved 100%** | **Lost** (full reboot) |
| **Wakeup Latency** | **< 1 ms** (immediate keystroke) | **~200 ms** (cold boot + BLE reconnection) |
| **Compatibility with Current Pins** | **Fully Compatible** (`GPIO 8, 9, 10, 20`) | **Incompatible** (cannot wake up from keys) |

> [!IMPORTANT]
> Because **GPIO 8, 9, 10, and 20** are in the `VDD3P3_CPU` domain, entering **Deep Sleep** on this pin layout will leave the chip permanently asleep with no way to wake up via key presses.
> 
> Therefore, the firmware defaults to **Light Sleep**, which achieves ~130 µA power draw with instantaneous response.

---

## 4. Hardware Recommendations for Custom PCB Design

If designing a custom PCB revision for Nimble 4:

1. **Migrate Keys to RTC GPIOs (0..5)**:
   - Wire the 4 keys to **`GPIO 0`, `GPIO 1`, `GPIO 2`, `GPIO 3`**.
   - These pins belong to the `VDD3P3_RTC` power domain and are completely free from boot strapping conflicts.
   - This enables true **Deep Sleep (~5 µA)**, allowing a 150 mAh LiPo battery to last over **2 years** on standby.
2. **External Pull-Ups**:
   - For lowest sleep current in deep sleep, place 100 kΩ pull-up resistors to 3.3V on each key line to avoid internal pull-up leakage.
3. **ESD Protection**:
   - Add TVS diode arrays (e.g. USBLC6-2SC6) on exposed mechanical switch pads to protect the microcontroller from electrostatic discharge.
