# Development

```{include} ../../CONTRIBUTING.md
```

## Video tutorial

A short video tutorial on how to build a custom firmware version ([YouTube mirror](https://youtu.be/WrPj2_DdfjI)):

<iframe width="720" height="405" src="https://rutube.ru/play/embed/1ff6fc7246260b3d404acebd0435d785?p=faQjyf7QWhT3bff2GDrReQ" frameBorder="0" allow="clipboard-write; autoplay" webkitAllowFullScreen mozallowfullscreen allowFullScreen></iframe>

## Compiling the source code

The firmware is built with [ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/) v6.0.2 and CMake. The libraries are git submodules. Do not clone them recursively: `esp-protocols` and `idf-extra-components` contain dozens of nested submodules that the build does not need. Run `make submodules` after cloning, or the two commands it consists of:

```shell
git submodule update --init
git submodule update --init --recursive components/esp_littlefs
```

1. [Install ESP-IDF](https://docs.espressif.com/projects/esp-idf/en/v6.0.2/esp32s3/get-started/index.html#installation) v6.0.2 and activate it in your shell (`. $IDF_PATH/export.sh`)
2. Go to the repository root folder
3. Compile and flash the firmware

```shell
idf.py -B build/ospi -D SDKCONFIG=build/ospi/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ospi" -D IDF_TARGET=esp32s3 -p COM_PORT flash
```

The hardware variant is selected by the second file in `SDKCONFIG_DEFAULTS`:

* `sdkconfig.ospi` - ATS Mini with octal PSRAM
* `sdkconfig.qspi` - ATS Mini with quad PSRAM
* `sdkconfig.lilygo` - [LILYGO T-Embed SI4732](hardware.md#lilygo-t-embed-si4732)

The firmware images are in the `build/<variant>` folder.

## Libraries

The libraries live in the `components` folder as git submodules:

* [LovyanGFX](https://github.com/lovyan03/LovyanGFX) - graphics and the display driver
* [esp_littlefs](https://github.com/joltwallet/esp_littlefs) - LittleFS file system
* [esp-protocols](https://github.com/espressif/esp-protocols) - mDNS
* [idf-extra-components](https://github.com/espressif/idf-extra-components) - QR code generator

The SI4732 driver (`components/si473x`) is part of the repository.

## Compile-time options

The options are in the `ATS-Neo` menu of `idf.py menuconfig`. The available options are:

* `ATS_HALF_STEP` - enable encoder half-steps (useful for EC11E encoder)
* `ATS_FPS_COUNTER` - show the FPS counter
* `ATS_BLE_TX_POWER_DBM` - Bluetooth LE TX power level in dBm (default: `0`). Possible values are `-24`, `-21`, `-18`, `-15`, `-12`, `-9`, `-6`, `-3`, `0`, `3`, `6`, `9`, `12`, `15`, `18`, and `20`.
* `ATS_WIFI_TX_POWER_QDBM` - Wi-Fi TX power level in units of 0.25 dBm (default: `68`, that is 17 dBm). For example `52` is 13 dBm and `74` is 18.5 dBm.

The hardware variant (`ATS_BOARD_*`) is chosen by the `sdkconfig.<variant>` file.

To set an option without the menu, put it into a file named `sdkconfig.local` and add it to the end of the `SDKCONFIG_DEFAULTS` list. For example, to enable the encoder half-steps and lower the Bluetooth LE and Wi-Fi TX power, create `sdkconfig.local` with the following content:

```
CONFIG_ATS_HALF_STEP=y
CONFIG_ATS_BLE_TX_POWER_DBM=-12
CONFIG_ATS_WIFI_TX_POWER_QDBM=52
```

and build with:

```shell
idf.py -B build/ospi -D SDKCONFIG=build/ospi/sdkconfig -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;sdkconfig.ospi;sdkconfig.local" -D IDF_TARGET=esp32s3 -p COM_PORT flash
```

## Using the make command

You can do all of the above using the `make` command as well. It picks up `sdkconfig.local` automatically:

```shell
VARIANT=ospi PORT=/dev/tty.usbmodem14401 make upload
```

## Decoding stack traces

To decode a stack trace (printed via serial port) use `idf.py monitor`, or the following tool: <https://esphome.github.io/esp-stacktrace-decoder/>

## Enabling the pre-commit hooks

1. Install `uv` <https://docs.astral.sh/uv/getting-started/installation/>
2. Run `uv sync`
3. run `uv run prek install`

## Adding a changelog entry

1. Install `uv` <https://docs.astral.sh/uv/getting-started/installation/>
2. Run `uv sync`
3. Create an entry:
   ```
   uv run towncrier create --edit ID.CATEGORY.md
   ```
   `ID` is an issue or a PR number, or `+STRING` if there is no issue/PR. `CATEGORY` is one of `added`, `changed`, `fixed`, etc. see the `tool.towncrier.type` sections in the `pyproject.toml` for the full list.

## Improving the documentation

1. Install `uv` <https://docs.astral.sh/uv/getting-started/installation/>
2. Run `uv sync`
3. Run a local webserver `uv run sphinx-autobuild docs/source docs/build` and open the http://127.0.0.1:8000 in a browser
4. Edit the Markdown files in `docs/source` folder and immediately see your changes reflected in the browser

## Theme editor

A terminal command <kbd>T</kbd> toggles a special mode that helps you pick the right colors faster without recompiling and flashing the firmware each time. When the theme editor is enabled, some screen elements are always visible (and various status icons change their state every couple of seconds):

![](_static/theme-editor.png)

Press <kbd>@</kbd> to print the current color theme to the serial console:

```shell
Color theme Default: x0000xFFFFxD69AxF800xD69Ax07E0xF800xF800xFFFFxFFFFx07E0xF800x001FxFFE0xD69AxD69AxD69Ax0000xD69AxD69AxF800xBEDFx0000xF800xFFFFxBEDFx105BxBEDFxBEDFxFFFFxD69AxF800xFFE0xD69AxFFFFxF800xC638
```

Then copy the theme to your favorite text editor, change the colors as you see (here is a handy [565 color picker](https://chrishewett.com/blog/true-rgb565-colour-picker/)).

To preview the theme, paste it to the serial console with the <kbd>^</kbd> character appended:

```shell
^x0000xFFFFxD69AxF800xD69Ax07E0xF800xF800xFFFFxFFFFx07E0xF800x001FxFFE0xD69AxD69AxD69Ax0000xD69AxD69AxF800xBEDFx0000xF800xFFFFxBEDFx105BxBEDFxBEDFxFFFFxD69AxF800xFFE0xD69AxFFFFxF800xC638
```

Once you are happy, add the resulting colors to `main/Themes.cpp`.

## Release process

1. Bump the `VER_APP` constant in the `main/Common.h` file
2. If the new version has a different preferences layout, bump `VER_SETTINGS`, `VER_BANDS`, `VER_MEMORIES`, or `VER_STORAGE` as well (it will force the corresponding preferences section reset)
3. Generate the CHANGELOG.md by running `uv run towncrier build --version X.XX`
4. Add and commit the changes with a message like "Release X.XX", then push them to the repository
5. Once the build is complete, download, flash and test it!
6. Tag the release and push the tag `git tag -a vX.XX -m 'Version X.XX' && git push --follow-tags` (the tag should start with `v`!)
