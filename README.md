# 🐉 DragonWheeze

> *"It's not DragonBreath. It's barely a dragon. It's a wheeze."*

**DragonWheeze** is open, native ESP-IDF firmware for the **Sovol SH01 Filament Dryer** running on an **ESP32-C3 Super Mini**. 

Unlike its flame-spewing sibling [`DragonBreath`](https://github.com/justinh-rahb/DragonBreath) (which rips out stock electronics to directly control mains SSRs, zero-cross TRIAC AC fans, and dual thermistors), **DragonWheeze** is a gloriously cursed, subordinate piggyback hack. It leaves the stock Sovol control board intact and uses optocouplers to electrically poke capacitive touch pads like a tiny, relentless phantom finger.

---

## ⚡ The Cursed Hardware Reality

The Sovol SH01 is a charmingly quirky piece of kit. To bring it into the `dragon-core` ecosystem without redesigning its mainboard from scratch, the hardware interface involves several mechanical and electrical compromises:

- **The Finger Simulator 3000**: Three **4N35 optocouplers** wired across the BS813A-1 touch-key IC. When the ESP32-C3 wants to press `M` (Mode), `A` (Adjust), or `P` (Power), it energizes an optocoupler LED for 200ms to ground the touch pad, tricking the Sovol chip into believing a human touched it.
- **Noise Mitigation**: 100kΩ resistors are hand-soldered directly onto the front panel's physical touch-spring joints so long wire runs don't cause false ghost presses.
- **Power Delivery Struggles**: The SH01's onboard 3.3V rail is far too weak to power an ESP32-C3 during Wi-Fi transients. Power is tapped from the 12V rail through a buck converter stepped down to ~5.8V fed into 5V/GND, stabilized with a 100µF decoupling capacitor to ensure reliable boot.
- **Blind Faith State Machine**: Space constraints inside the case mean we can only monitor the physical Power button (via BS813A-1 pin 4). The `M` and `A` physical buttons and the 7-segment LCD display are completely unobservable. To set a target temperature or time deterministically, DragonWheeze executes a **"Reset-Before-Automation"** routine: it turns the dryer OFF, turns it ON (resetting it to 40°C / 6h base state), and then rapidly taps `M` and `A` to set your desired profile.
- **The Mysterious E0 Error**: The SH01 control board occasionally throws an `E0` I2C error when it feels overwhelmed. DragonWheeze mitigates this by throttling auxiliary AHT20 sensor polling (default 10s) and logging bus contention events to an in-memory ring buffer (`dc_evlog`) for diagnostic support.

---

## 🏗️ Architecture: Subordinate Supervisory Control

DragonWheeze runs on top of [`dragon-core`](https://github.com/justinh-rahb/dragon-core) (`dc_wifi`, `dc_portal`, `dc_ui`, `dc_mqtt`, `dc_evlog`), replacing brittle Home Assistant `input_select`/`input_boolean`/`time_pattern` automations with local firmware state:

```
[ Home Assistant / Local Web UI ]
               │ (MQTT / HTTP)
               ▼
┌────────────────────────────────────────────────────────┐
│ DragonWheeze Firmware (ESP32-C3 Super Mini)            │
│  - Owns local timers, preset profiles (PLA/PETG/TPU/ABS)│
│  - Reads auxiliary ambient temp & humidity (AHT20)     │
│  - Executes 4N35 optocoupler pulse macros              │
└───────────────────────────┬────────────────────────────┘
                            │ (Optocoupler Taps)
                            ▼
┌────────────────────────────────────────────────────────┐
│ Sovol SH01 Stock Control Board                         │
│  - Controls 12V resistive PCB heater element           │
│  - Reads internal thermistor & performs thermal safety │
│  - Drives 7-segment display                            │
└────────────────────────────────────────────────────────┘
```

The stock Sovol controller handles closed-loop heating of the resistive PCB element and thermal safety; DragonWheeze acts as an intelligent remote control that publishes state to Home Assistant.

---

## 📌 Pinout (ESP32-C3 Super Mini)

| Target | GPIO Pin | Notes |
|---|---|---|
| **Optocoupler M** | `GPIO 4` | Mode button pulse (active HIGH) |
| **Optocoupler A** | `GPIO 5` | Adjust button pulse (active HIGH) |
| **Optocoupler P** | `GPIO 6` | Power button pulse (active HIGH) |
| **Power Sense** | `GPIO 7` | BS813A-1 pin 4 tap (idle HIGH, active LOW) |
| **AHT20 SDA** | `GPIO 0` | I2C Data (auxiliary ambient sensor) |
| **AHT20 SCL** | `GPIO 1` | I2C Clock (auxiliary ambient sensor) |

---

## 🚀 Preset Profiles

| Preset | Target Temp | Target Duration |
|---|---|---|
| **PLA** | 45°C | 6 hours |
| **PETG** | 50°C | 8 hours |
| **TPU** | 50°C | 10 hours |
| **ABS** | 50°C | 12 hours |

---

## 🛠️ Building & Flashing

Requires ESP-IDF v5.x:

```bash
idf.py set-target esp32c3
idf.py build
idf.py flash monitor
```

---

## 📜 License

MIT License. See [LICENSE](LICENSE).
