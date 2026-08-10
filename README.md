# DragonWheeze

Native ESP-IDF firmware for the **Sovol SH01 Filament Dryer** running on an **ESP32-C3 Super Mini**, consuming shared components from [`dragon-core`](https://github.com/justinh-rahb/dragon-core).

The name is a nod to the fact that the SH01 is a lighter, simpler entry in the Dragon family lineup compared to [`DragonBreath`](https://github.com/justinh-rahb/DragonBreath).

---

## Attribution & Acknowledgments

This firmware builds upon the excellent hardware reverse-engineering writeup by **Simply Maker**:
👉 **[Easily Integrate SOVOL Filament Dryer to Home Assistant with ESP32 — Simply Maker](https://simplymaker.net/electronics/easily-integrate-sovol-filament-dryer-to-home-assistant-with-esp32/)**

The hardware interface strategy—using 4N35 optocouplers across the BS813A-1 touch IC, 100kΩ noise-suppression resistors on touch-spring joints, 12V buck conversion, and power button monitoring—is directly derived from Simply Maker's original analysis.

Whereas the original build used ESPHome as a basic GPIO bridge with Home Assistant helpers (`input_boolean`, `input_select`) and automations handling the sequence logic, **DragonWheeze** ports that control logic into native C/C++ firmware running on the ESP32-C3 itself.

---

## Hardware Interface & Implementation Details

- **Touch Button Emulation**: Three **4N35 optocouplers** simulate physical touches on the `M` (Mode), `A` (Adjust), and `P` (Power) pads by grounding each button trace for 200ms.
- **Noise Mitigation**: 100kΩ resistors soldered to each touch-spring joint prevent false triggers caused by wire inductance.
- **Power Delivery**: Powered from the SH01's 12V rail via a buck converter stepped down to ~5.8V fed into 5V/GND, with a 100µF capacitor across 5V/GND for boot stability.
- **Power Button Sensing**: Pin 4 off the BS813A-1 touch IC feeds an ESP32 GPIO (idle HIGH, drops LOW on physical press) for hardware button monitoring.
- **Environment Sensing**: Onboard AHT20 sensor read over I2C via a 4-pin connector.
- **On-Device State Machine**: Because physical `M` and `A` taps cannot be read directly, sequence commands use a deterministic **reset-before-automation** strategy (Power OFF $\rightarrow$ ON to reach known 40°C/6h base state, followed by target `M`/`A` pulses).
- **I2C E0 Error Diagnostics**: Throttles AHT20 polling intervals (default 10s) and logs bus contention events to `dc_evlog` to help characterize and mitigate the SH01 mainboard's intermittent `E0` error.

---

## Architecture: Subordinate Supervisory Controller

DragonWheeze uses [`dragon-core`](https://github.com/justinh-rahb/dragon-core) (`dc_wifi`, `dc_portal`, `dc_ui`, `dc_mqtt`, `dc_evlog`) to run state control locally on-device:

```
[ Home Assistant / Local Web UI ]
               │ (MQTT / HTTP)
               ▼
┌────────────────────────────────────────────────────────┐
│ DragonWheeze Firmware (ESP32-C3 Super Mini)            │
│  - Owns local timers & presets (PLA/PETG/TPU/ABS)      │
│  - Reads auxiliary ambient temp & humidity (AHT20)     │
│  - Executes optocoupler pulse macro sequences          │
└───────────────────────────┬────────────────────────────┘
                            │ (4N35 Optocoupler Taps)
                            ▼
┌────────────────────────────────────────────────────────┐
│ Sovol SH01 Stock Control Board                         │
│  - Regulates 12V resistive PCB heating element         │
│  - Handles closed-loop thermistor safety               │
│  - Drives front panel & 7-segment display              │
└────────────────────────────────────────────────────────┘
```

---

## Pinout (ESP32-C3 Super Mini)

| Line / Peripheral | GPIO Pin | Details |
|---|---|---|
| **Optocoupler M** | `GPIO 4` | Mode button pulse (active HIGH) |
| **Optocoupler A** | `GPIO 5` | Adjust button pulse (active HIGH) |
| **Optocoupler P** | `GPIO 6` | Power button pulse (active HIGH) |
| **Power Sense** | `GPIO 7` | BS813A-1 pin 4 tap (idle HIGH, active LOW) |
| **AHT20 SDA** | `GPIO 0` | I2C Data (4.7kΩ pullup) |
| **AHT20 SCL** | `GPIO 1` | I2C Clock (4.7kΩ pullup) |

---

## Preset Profiles

| Preset | Target Temp | Target Duration |
|---|---|---|
| **PLA** | 45°C | 6 hours |
| **PETG** | 50°C | 8 hours |
| **TPU** | 50°C | 10 hours |
| **ABS** | 50°C | 12 hours |

---

## Building & Flashing

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

---

## License

MIT License. See [LICENSE](LICENSE).
