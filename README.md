# 🐉 DragonWheeze

> *"It's not DragonBreath. It's barely a dragon. It's a wheeze."*

**DragonWheeze** is open, native ESP-IDF firmware for the **Sovol SH01 Filament Dryer** running on an **ESP32-C3 Super Mini**, built on shared components from [`dragon-core`](https://github.com/justinh-rahb/dragon-core).

Unlike its flame-spewing sibling [`DragonBreath`](https://github.com/justinh-rahb/DragonBreath) (which rips out stock electronics to directly control mains SSRs, zero-cross TRIAC AC fans, and dual thermistors), **DragonWheeze** is a gloriously cursed, subordinate piggyback hack. It leaves the stock Sovol control board intact and uses optocouplers to electrically poke capacitive touch pads like a tiny, relentless phantom finger. The name is a nod to the SH01 being a lighter, simpler entry in the Dragon family lineup.

---

## Attribution & Acknowledgments

This firmware builds on the excellent hardware reverse-engineering writeup by **Simply Maker**:

👉 **[Easily Integrate SOVOL Filament Dryer to Home Assistant with ESP32 — Simply Maker](https://simplymaker.net/electronics/easily-integrate-sovol-filament-dryer-to-home-assistant-with-esp32/)**

The hardware interface strategy (4N35 optocouplers across the BS813A-1 touch IC, 100kΩ noise-suppression resistors on the touch-spring joints, 12V buck conversion, and power button sensing) is directly derived from Simply Maker's original analysis.

Where the original build used ESPHome as a basic GPIO bridge with Home Assistant helpers (`input_boolean`, `input_select`) and automations handling the sequence logic, **DragonWheeze** ports that control logic into native C/C++ firmware running on the ESP32-C3 itself.

---

## ⚡ The Cursed Hardware Reality

The Sovol SH01 is a charmingly quirky piece of kit. Bringing it into the `dragon-core` ecosystem without redesigning its mainboard from scratch means living with a few mechanical and electrical compromises:

- **The Finger Simulator 3000**: Three **4N35 optocouplers** wired across the BS813A-1 touch-key IC. When the ESP32-C3 wants to press `M` (Mode), `A` (Adjust), or `P` (Power), it energizes an optocoupler LED for 200ms to ground the touch pad, tricking the Sovol chip into believing a human touched it.
- **Noise Mitigation**: 100kΩ resistors are hand-soldered directly onto the front panel's touch-spring joints so long wire runs don't cause false ghost presses.
- **Power Delivery Struggles**: The SH01's onboard 3.3V rail is far too weak to power an ESP32-C3 during Wi-Fi transients. Power is tapped from the 12V rail through a buck converter stepped down to ~5.8V fed into 5V/GND, stabilized with a 100µF decoupling capacitor for reliable boot.
- **Blind Faith State Machine**: Space constraints mean we can only monitor the physical Power button (via BS813A-1 pin 4). The `M` and `A` physical buttons and the 7-segment display are completely unobservable. To set a target temperature or time deterministically, DragonWheeze runs a **reset-before-automation** routine: power OFF, power ON (landing on the known 40°C / 6h base state), then rapid `M`/`A` pulses to dial in the target profile.
- **The E0 Error — RESOLVED (use a dedicated sensor)**: Tapping the ESP onto the SH01 mainboard's own AHT20 I2C bus causes the board to throw `E0`. This was confirmed on hardware: with the ESP's I2C lines (GPIO0/GPIO1) physically disconnected there is **no E0**; reconnecting them brings it back **immediately**, even with the ESP's I2C driver never initialized. So it is electrical (pin capacitance + ESD leakage loading the mainboard's weak pull-ups and corrupting its reads), not firmware or polling rate. **Recommendation: do not splice into the mainboard's sensor bus.** Either (a) run without ambient sensing — the firmware defaults AHT20 polling to **disabled (`0`)**, so it never touches the bus, or (b) wire the ESP its **own** AHT20 on GPIO0/GPIO1 (isolated from the mainboard) and set a non-zero poll interval on the web setup page. The interval is NVS-persisted and logs bus stats to `dc_evlog` when enabled.

---

## Architecture: Subordinate Supervisory Controller

DragonWheeze uses [`dragon-core`](https://github.com/justinh-rahb/dragon-core) (`dc_wifi`, `dc_portal`, `dc_ui`, `dc_mqtt`, `dc_evlog`) to run state control locally on-device:

```
[ Home Assistant / Local Web UI ]
               │ (MQTT / HTTP)
               ▼
┌────────────────────────────────────────────────────────┐
│ DragonWheeze Firmware (ESP32-C3 Super Mini)             │
│  - Owns local timers & presets (PLA/PETG/TPU/ABS)       │
│  - Reads auxiliary ambient temp & humidity (AHT20)      │
│  - Executes optocoupler pulse macro sequences           │
└───────────────────────────┬────────────────────────────┘
                            │ (4N35 Optocoupler Taps)
                            ▼
┌────────────────────────────────────────────────────────┐
│ Sovol SH01 Stock Control Board                          │
│  - Regulates 12V resistive PCB heating element          │
│  - Handles closed-loop thermistor safety                │
│  - Drives front panel & 7-segment display                │
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
