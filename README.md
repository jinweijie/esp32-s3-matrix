# ESP32-S3 Matrix

Materials for the [Waveshare ESP32-S3-Matrix](https://www.waveshare.net/wiki/ESP32-S3-Matrix): 8×8 RGB LEDs on **GPIO 14**, ESP32-S3, 4 MB flash.

This repository contains **ESP-IDF** apps only (no bundled Arduino `demo/` tree). Official Arduino sketches from Waveshare live on their wiki and examples pages.

| Folder               | What it is                                                                                                                                 |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ |
| `src/color_matrix/`  | **ESP-IDF** port of the **Color** flow (lookup table, GPIO 14).                                                                          |
| `src/font_matrix/`   | **ESP-IDF** port of **Font**: scrolling text (Adafruit 5×7 font, NeoMatrix-style layout).                                                   |
| `src/game_matrix/`   | **ESP-IDF** port of **Game**: QMI8658 accelerometer (I2C SDA 11 / SCL 12) moves a dot on the matrix (GPIO 14).                           |
| `src/http_matrix/`   | **ESP-IDF** **HTTP**: Soft AP + web UI + scrolling text, Chinese subset, and JSON-style config API.                                        |
| `src/snake_matrix/`  | **Snake** on the LED matrix: tilt QMI8658 to steer (same I2C / GPIO as `game_matrix`).                                                    |

---

## 1. Prerequisites

- **ESP-IDF** (this project was tested with **v5.3.x**; adjust paths if you use another version).
- **Python 3.8+** available as the `python` command (required by ESP-IDF’s `export.ps1`).
- **Git** (for ESP-IDF and submodules).
- **USB cable** and a **COM port** for the board (Device Manager → Ports).

**Windows:** If `python` opens the Microsoft Store instead of a real interpreter, turn off the **App execution aliases** for `python.exe` / `python3.exe` under _Settings → Apps → Advanced app settings → App execution aliases_, or install Python and add it to your **user** `PATH` before the Store entries.

---

## 2. Install ESP-IDF (pick one)

Use any official method; all of them install the toolchain and (usually) the IDF Python venv.

1. **Espressif IDF extension for VS Code** — Express install, then use the extension’s terminal / environment.
2. **ESP-IDF Installation Manager (EIM)** — GUI or CLI; for online installs from China, prefer **GitHub** mirrors if JiHuLab asks for login.
3. **Offline / online Windows installer** from [Espressif’s download area](https://dl.espressif.com/esp-idf/).

After install, you should have something like:

`%USERPROFILE%\Espressif\v5.3.2\esp-idf`

---

## 3. One-time fixes (only if something fails)

Open **PowerShell** (or **ESP-IDF PowerShell** / VS Code IDF terminal where `python` and `idf.py` work).

**Missing compiler tools** (errors about `xtensa-esp-elf`, `cmake`, `ninja`, …):

```powershell
python "$env:IDF_PATH\tools\idf_tools.py" install
```

**Missing IDF Python venv** (error that  
`%USERPROFILE%\.espressif\python_env\idf5.3_py3.11_env\Scripts\python.exe` does not exist):

```powershell
python "$env:IDF_PATH\tools\idf_tools.py" install-python-env
```

If `IDF_PATH` is not set, replace `$env:IDF_PATH` with your `esp-idf` folder, for example:

```powershell
python "C:\Users\YOURNAME\Espressif\v5.3.2\esp-idf\tools\idf_tools.py" install-python-env
```

---

## 4. Activate ESP-IDF in a new terminal

**PowerShell** (adjust the path to match your install):

```powershell
cd C:\Users\YOURNAME\Espressif\v5.3.2\esp-idf
.\export.ps1
```

Or use the **“ESP-IDF PowerShell”** shortcut created by the installer.

Check:

```powershell
idf.py --version
```

---

## 5. Build and flash ESP-IDF apps (`color_matrix`, `font_matrix`, `game_matrix`, `http_matrix`, `snake_matrix`)

These apps live under `src/<name>/`. Each has its own `build/` output and its own `sdkconfig` after you configure the target.

### Before you run any IDF app

1. **Activate ESP-IDF in this terminal** (see **section 4**). Check with `idf.py --version`.
2. **Stay online for the first build** (and any clean rebuild): the [IDF Component Manager](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-guides/tools/idf-component-manager.html) downloads **`espressif/led_strip`** from the registry using `main/idf_component.yml`. Without network, configure a PyPI / component mirror or vendor the dependency yourself.
3. **USB**: plug the board in and note the **COM port** (Device Manager → _Ports (COM & LPT)_). Use that instead of `COM3` below.

### Every build (typical flow)

From a shell where `idf.py` works:

```powershell
cd <PATH_TO_THIS_REPO>\src\<app_folder>
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

- **`set-target esp32s3`**: run once per app folder (or again after `idf.py fullclean`). Picking the wrong target will misconfigure the chip.
- **`build`**: must be run from the app directory (for example `color_matrix`, `font_matrix`, `game_matrix`, `http_matrix`, or `snake_matrix`) so CMake sees `CMakeLists.txt` and `main/idf_component.yml`.
- **`flash monitor`**: builds if needed, flashes the firmware, then opens the serial console. Exit the monitor with **Ctrl+]** (then **Ctrl+C** if your IDF build still shows a prompt). To flash without opening the monitor, use `idf.py -p COM3 flash`.

If you switch between apps under **`src/`**, use **`cd`** into the correct folder before `build` / `flash`; you do not need to delete another app’s `build/` folder.

### `color_matrix` (Color)

```powershell
cd <PATH_TO_THIS_REPO>\src\color_matrix
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**Wrong LED colors:** in `main/color_matrix_main.c`, try changing  
`LED_STRIP_COLOR_COMPONENT_FMT_RGB` to `LED_STRIP_COLOR_COMPONENT_FMT_GRB`, then rebuild and flash.

### `font_matrix` (Font)

```powershell
cd <PATH_TO_THIS_REPO>\src\font_matrix
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

Uses **GRB** order to match typical NeoPixel **GRB** wiring on this board. If text looks wrong, try `LED_STRIP_COLOR_COMPONENT_FMT_RGB` in `main/font_matrix_main.c`.

### `game_matrix` (Game)

```powershell
cd <PATH_TO_THIS_REPO>\src\game_matrix
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**I2C:** QMI8658 uses **SDA GPIO 11**, **SCL GPIO 12** (same pinout as common Waveshare examples). If the IMU is not detected at boot, check wiring and that your board matches this pinout.

**LED mapping:** `NEO_RGB`-style channel order in firmware (`LED_STRIP_COLOR_COMPONENT_FMT_RGB` in `main/game_matrix_main.c`). If colors are wrong, try **GRB** like `font_matrix`.

### `http_matrix` (HTTP)

```powershell
cd <PATH_TO_THIS_REPO>\src\http_matrix
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**Wi‑Fi:** Join AP **`ESP32-S3-Matrix`** / password **`adminadmin`**, then open **http://10.50.50.1/** (static AP address). The web UI sets **message text**, **speed** (1–10), **display mode**, **marquee direction** (`left`, `right`, `up`, `down`), **one-character zoom** (`none`, `in`, `out`), **color** presets, optional **rainbow** on lit pixels, and **Turn off LEDs**. **Endpoints:** **`/`** (UI), **`/getData`**, **`/SendData?data=…`**, **`/api/config`** with query keys including **`speed`**, **`mode`**, **`color`**, **`rainbow`**, **`direction`**, **`zoom`** — `mode` is `marquee`, `onechar`, `static`, or `fill`; `direction` applies to marquee scrolling (horizontal: left/right; vertical: down = top-to-bottom, up = bottom-to-top); `zoom` is `none`, `in`, or `out` (one-character mode: eight-frame grow/shrink per glyph). Legacy **`/RGBOn`** / **`/RGBOff`** compatibility routes still work (green lamp vs. clear + marquee).

**Chinese (UTF-8):** Send **UTF-8** text from the browser (_encodeURIComponent_ handles Chinese). **`/SendData`** percent-decodes `data`, resets one-character mode, and enables animation so new text appears immediately. The matrix uses **`glcdfont`** for ASCII and an **8×8 Chinese subset** in **`main/cjk8x8.c`**. Codepoints **not** in that table show **`?`**. Add `{ Unicode, { 8 row bytes } }` to **`k_glyphs[]`** (sorted by codepoint) to extend coverage.

**LED layout:** **TOP + LEFT + ROWS + PROGRESSIVE** (NeoMatrix-style), implemented as strip index **`y * 8 + x`** in `main/http_matrix_main.c`. Color order is **GRB**.

### `snake_matrix` (Snake)

```powershell
cd <PATH_TO_THIS_REPO>\src\snake_matrix
idf.py set-target esp32s3
idf.py build
idf.py -p COM3 flash monitor
```

**Play:** Hold the board level; **tilt** left/right/up/down to choose direction (stronger axis wins). The snake moves on a fixed timer; **red** is food, **bright green** is the head, **dim green** is the body. **Wall** or **self** collision shows **score** (foods eaten = length − 3) as amber digits for **3 seconds**, then restarts. Fill all **64** cells to win (score **61** shown the same way).

Change **`SCORE_DISPLAY_MS`** in `snake_matrix_main.c` to adjust how long the score stays on screen.

**Tuning:** In `main/snake_matrix_main.c`, adjust **`MOVE_MS`**, **`TILT_DEAD`** / **`TILT_STRONG`**, and **`BRIGHTNESS`**. **`SNAKE_TILT_SWAP_AX_AY`** **`1`**; **`SNAKE_TILT_MIRROR_Y`** **`1`** (up/down); **`SNAKE_TILT_MIRROR_X`** **`1`** (left/right). Flip any mirror to **`0`** if that axis feels reversed on your unit. Hardware: QMI8658 **SDA 11 / SCL 12**, LEDs **GPIO 14**, **`LED_STRIP_COLOR_COMPONENT_FMT_RGB`**.

**LED brightness (doc + tuning):** the [Waveshare wiki](https://www.waveshare.net/wiki/ESP32-S3-Matrix) says not to set the matrix **too bright**—high brightness can make the board **overheat and be damaged** (see the “Please note the lamp brightness…” / 灯珠亮度 warnings in their example pages). They do not give a single safe number; treat it as “keep it moderate,” especially for long runs.

In these IDF projects, overall dimming is a single scale **`BRIGHTNESS`** in C (roughly 1–255, **lower = dimmer**):

| App            | File                                           | Symbol               |
| -------------- | ---------------------------------------------- | -------------------- |
| `color_matrix` | `src/color_matrix/main/color_matrix_main.c`    | `#define BRIGHTNESS` |
| `font_matrix`  | `src/font_matrix/main/font_matrix_main.c`      | `#define BRIGHTNESS` |
| `game_matrix`  | `src/game_matrix/main/game_matrix_main.c`      | `#define BRIGHTNESS` |
| `http_matrix`  | `src/http_matrix/main/http_matrix_main.c`      | `#define BRIGHTNESS` |
| `snake_matrix` | `src/snake_matrix/main/snake_matrix_main.c`    | `#define BRIGHTNESS` |

Defaults here are set **lower** than typical Arduino “full brightness” examples to reduce heat. If it is too dim, increase `BRIGHTNESS` a little, rebuild, and flash; if it is still too bright, lower it (e.g. **10–20** for a dim night display).

---

## 6. Arduino IDE (optional)

Waveshare’s official **Arduino** examples (NeoPixel, NeoMatrix, GFX, QMI8658, etc.) are documented on the [Waveshare wiki](https://www.waveshare.net/wiki/ESP32-S3-Matrix). Use **Arduino IDE** or **Arduino CLI** with the **esp32** board package and board such as **ESP32S3 Dev Module**. The RGB data pin in their sketches is **GPIO 14**.

---

## 7. Useful links

- [Waveshare ESP32-S3-Matrix wiki](https://www.waveshare.net/wiki/ESP32-S3-Matrix)
- [ESP-IDF get started (Windows)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/get-started/windows-setup.html)
