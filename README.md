# RGB underglow status widget for ZMK

> [!IMPORTANT]
> This module targets ZMK configurations that use `CONFIG_ZMK_RGB_UNDERGLOW`.
> It uses the keyboard's RGB underglow/backlight strip for battery, BLE, and layer status indication.

This is a [ZMK module](https://zmk.dev/docs/features/modules) that shows keyboard status on an RGB underglow strip. It can temporarily borrow the whole strip, or use dedicated underglow status pixels when the ZMK underglow backend supports status channels.

## Features

### Battery Status

- Shows battery status on boot by default. Disable with `CONFIG_RGBLED_WIDGET_BOOT_SHOW_BATTERY=n`.
- Provides `&ind_bat` for showing battery status on demand from a keymap.
- When showing the current-side battery, the widget samples the configured `zmk,battery` sensor
  directly using ZMK's selected state-of-charge or lithium-voltage fetch mode. This avoids ZMK's
  cached value becoming stale while the keyboard is idle and charging.
- Battery sampling and indication preparation run asynchronously on ZMK's low-priority work queue,
  so `&ind_bat` does not block key processing. If a cached value is not ready, retries use delayed
  work and release the queue between attempts so RGB rendering can continue.
- Uses the configured battery colors:
  - Green: at or above `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH`
  - Yellow: at or above `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW`
  - Red: below `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW`
  - Magenta: current half battery not detected
- Low/red battery display is solid red in the normal battery indicator path.
- Critical battery changes still blink red as an alert when the level is at or below `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL`.
- Enable `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS` to show battery level on five configured underglow pixels instead of borrowing the whole strip. The indicator fills the bar one segment at a time, then holds the final level until the battery indication timeout ends.

### Split Battery Levels

For split keyboards, each half reports its own battery by default.

On a split central, you can also include peripheral battery levels:

- `CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS`: use self plus peripheral levels
- `CONFIG_RGBLED_WIDGET_BATTERY_SHOW_ONLY_PERIPHERALS`: use peripheral levels only

When peripheral levels are included, the widget displays the lowest available non-zero level. A peripheral level of `0` is treated as undetermined and skipped, so a sleeping or disconnected peripheral does not show as the magenta missing-battery state. Magenta is reserved for a current-side battery that cannot be detected.

### Connection Status

- Shows connectivity status on boot by default. Disable with `CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY=n`.
- Provides `&ind_con` for showing connectivity status on demand from a keymap.
- Uses the configured connectivity colors:
  - Blue: connected
  - Yellow: advertising/open
  - Red: disconnected
  - Cyan: USB endpoint active when `CONFIG_RGBLED_WIDGET_CONN_SHOW_USB=y`
- Enable `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL` to show connectivity on a single underglow status pixel.
- Enable `CONFIG_RGBLED_WIDGET_PAIRING_LED` to blink the discrete LED referenced by the `led-blue`
  devicetree alias with the same cadence as the underglow pairing indication. A node labeled
  `blue_led` is also accepted for compatibility. Unlike the underglow, this LED remains active in
  the idle state and turns off only when pairing ends or the keyboard actually enters sleep.
- BLE profile status pixels can be configured per profile with `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_0` through `_4`.
- When possible, the widget maps the active BLE profile to the underglow pixel for the key bound to `&bt BT_SEL <profile>`.
- Advertising and disconnected states blink; connected states show solid color for the configured duration.
- When `CONFIG_ZMK_SLEEP=y`, the underglow indicator for an open profile blinks until it connects or
  inactivity reaches `CONFIG_ZMK_IDLE_SLEEP_TIMEOUT`. The widget stops that indication at the
  timeout even while USB power prevents deep sleep, and resumes it on later activity if the profile
  is still open. The discrete pairing LED intentionally continues during USB-powered idle because
  it stops only on a real sleep transition.
- Duplicate connectivity indications are suppressed when the state has not changed.

### Layer State

You can pick one layer indication mode. Both are off by default.

- `CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE`: highlight keys defined on the highest active layer when the layer changes.
- `CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS`: keep a configured color active while a layer is the highest active layer.

For `CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE`, the widget highlights the keys defined directly on the highest active layer while RGB underglow/backlight is on. Transparent bindings and layer-switch bindings are skipped.

Layer indicators only run on non-split keyboards and split centrals because peripheral halves are not layer-aware.

### Underglow Behavior

Status indicators temporarily borrow the underglow strip and then restore the previous underglow state, including on/off state, color, and effect. If status-channel pixel APIs are unavailable, single-pixel and five-pixel status indicators fall back to whole-strip indication.

## Installation

Add this module to your `config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: tomahawk-keyboards
      url-base: https://github.com/Tomahawk-Keyboards
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-rgbled-widget
      remote: tomahawk-keyboards
      revision: main
  self:
    path: config
```

Then include the behavior definitions in your keymap if you want on-demand indicators:

```dts
#include <behaviors/rgbled_widget.dtsi>
```

## Basic Configuration

Enable ZMK RGB underglow and the widget in your keyboard `.conf`:

```ini
CONFIG_ZMK_RGB_UNDERGLOW=y
CONFIG_RGBLED_WIDGET=y
```

Optional startup behavior:

```ini
CONFIG_RGBLED_WIDGET_BOOT_SHOW_BATTERY=y
CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY=y
```

Optional discrete pairing LED:

```ini
CONFIG_RGBLED_WIDGET_PAIRING_LED=y
```

The board should expose that LED through the standard `led-blue` devicetree alias. Boards that
already provide the alias need no overlay. A GPIO LED can be added in an overlay like this:

```dts
#include <zephyr/dt-bindings/gpio/gpio.h>

/ {
    aliases {
        led-blue = &pairing_led;
    };

    leds {
        compatible = "gpio-leds";

        pairing_led: pairing-led {
            gpios = <&gpio0 15 GPIO_ACTIVE_HIGH>;
        };
    };
};
```

Replace the GPIO controller, pin, and polarity with values appropriate for the board. For backward
compatibility, the widget also accepts an enabled node labeled `blue_led` without an alias.

Timeout-based pairing underglow shutdown requires ZMK sleep support:

```ini
CONFIG_ZMK_SLEEP=y
CONFIG_ZMK_IDLE_SLEEP_TIMEOUT=900000
```

Optional dedicated status pixels:

```ini
CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL=y
CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX=0

CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS=y
CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_0=6
CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_1=7
CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_2=8
CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_3=9
CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_4=10
```

## Showing Status On Demand

This module defines keymap behaviors for battery, connectivity, and layer status:

```dts
/ {
    keymap {
        some_layer {
            bindings = <
                &ind_bat  // indicate battery level
                &ind_con  // indicate connectivity status
                &ind_lyr  // indicate current layer
            >;
        };
    };
};
```

The behavior runs on every keyboard half where `CONFIG_RGBLED_WIDGET=y` is enabled, so flash all relevant halves after changing widget settings.

## Configuration Reference

<details>
<summary>General</summary>

| Name | Description | Default |
| --- | --- | --- |
| `CONFIG_RGBLED_WIDGET` | Enable the RGB underglow widget | `n` |
| `CONFIG_RGBLED_WIDGET_INTERVAL_MS` | Minimum wait duration between indicator steps | `500` |
| `CONFIG_RGBLED_WIDGET_BOOT_SHOW_BATTERY` | Show battery status during boot | `y` |
| `CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY` | Show connectivity status during boot | `y` |

</details>

<details>
<summary>Battery</summary>

| Name | Description | Default |
| --- | --- | --- |
| `CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS` | Duration of battery indication | `2000` |
| `CONFIG_RGBLED_WIDGET_BATTERY_FILL_STEP_MS` | Battery status pixel fill animation step duration | `100` |
| `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH` | High battery threshold | `80` |
| `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW` | Low battery threshold | `20` |
| `CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL` | Critical battery threshold for warning blinks | `5` |
| `CONFIG_RGBLED_WIDGET_BATTERY_COLOR_HIGH` | Color for high battery level | Green (`2`) |
| `CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MEDIUM` | Color for medium battery level | Yellow (`3`) |
| `CONFIG_RGBLED_WIDGET_BATTERY_COLOR_LOW` | Color for low battery level | Red (`1`) |
| `CONFIG_RGBLED_WIDGET_BATTERY_COLOR_CRITICAL` | Color for critical battery warning | Red (`1`) |
| `CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING` | Color for battery not detected | Magenta (`5`) |
| `CONFIG_RGBLED_WIDGET_BATTERY_SHOW_SELF` | Indicate self battery only | `n` |
| `CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS` | On a split central, include peripheral battery levels | `n` |
| `CONFIG_RGBLED_WIDGET_BATTERY_SHOW_ONLY_PERIPHERALS` | On a split central, show peripheral battery levels only | `n` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS` | Use five RGB underglow pixels for battery level | `n` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_0` | First battery segment pixel index | `0` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_1` | Second battery segment pixel index | `1` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_2` | Third battery segment pixel index | `2` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_3` | Fourth battery segment pixel index | `3` |
| `CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_4` | Fifth battery segment pixel index | `4` |

</details>

<details>
<summary>Connectivity</summary>

| Name | Description | Default |
| --- | --- | --- |
| `CONFIG_RGBLED_WIDGET_CONN_BLINK_MS` | Duration of BLE connection status indication | `1000` |
| `CONFIG_RGBLED_WIDGET_CONN_SHOW_USB` | Show USB indicator instead of BLE status when USB has priority | `n` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL` | Use a single RGB underglow pixel for connectivity status | `n` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` | Default connectivity status pixel index | `0` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_0` | Connectivity status pixel for BLE profile 0 | `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_1` | Connectivity status pixel for BLE profile 1 | `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_2` | Connectivity status pixel for BLE profile 2 | `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_3` | Connectivity status pixel for BLE profile 3 | `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` |
| `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_4` | Connectivity status pixel for BLE profile 4 | `CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX` |
| `CONFIG_RGBLED_WIDGET_PAIRING_LED` | Blink the `led-blue` alias (or legacy `blue_led`) while pairing | `n` |
| `CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED` | Color for connected BLE status | Blue (`4`) |
| `CONFIG_RGBLED_WIDGET_CONN_COLOR_ADVERTISING` | Color for advertising BLE status | Yellow (`3`) |
| `CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED` | Color for disconnected BLE status | Red (`1`) |
| `CONFIG_RGBLED_WIDGET_CONN_COLOR_USB` | Color for USB endpoint active | Cyan (`6`) |

</details>

<details>
<summary>Layers</summary>

| Name | Description | Default |
| --- | --- | --- |
| `CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE` | Highlight keys defined on the highest active layer when the layer changes | `n` |
| `CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS` | Duration for layer highlights | `100` |
| `CONFIG_RGBLED_WIDGET_LAYER_COLOR` | Color for layer highlights | Cyan (`6`) |
| `CONFIG_RGBLED_WIDGET_LAYER_DEBOUNCE_MS` | Delay after layer change before indicating | `100` |
| `CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS` | Keep a configured color active for each layer | `n` |
| `CONFIG_RGBLED_WIDGET_LAYER_0_COLOR` through `CONFIG_RGBLED_WIDGET_LAYER_31_COLOR` | Per-layer colors for color-based layer indication | Layer 0 black, 1 red, 2 green, 3 yellow, 4 blue, 5 magenta, 6 cyan, 7 white, 8-31 black |

</details>

<details>
<summary>Color Values</summary>

| Color | Value |
| --- | --- |
| Black/off | `0` |
| Red | `1` |
| Green | `2` |
| Yellow | `3` |
| Blue | `4` |
| Magenta | `5` |
| Cyan | `6` |
| White | `7` |

</details>

## Legacy Compatibility

Older versions of this module used discrete GPIO RGB LEDs and the `rgbled_adapter` shield. The current widget uses ZMK RGB underglow instead. The behavior binding still accepts `led-gpios` as an ignored compatibility property for older devicetree nodes, but new configurations should use `CONFIG_ZMK_RGB_UNDERGLOW` and underglow pixel settings instead.
