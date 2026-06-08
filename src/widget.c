#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <errno.h>
#include <string.h>

#include <dt-bindings/zmk/bt.h>
#include <zmk/battery.h>
#include <zmk/behavior.h>
#include <zmk/ble.h>
#include <zmk/endpoints.h>
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/events/endpoint_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/events/split_peripheral_status_changed.h>
#include <zmk/events/activity_state_changed.h>
#include <zmk/keymap.h>
#include <zmk/matrix.h>
#include <zmk/rgb_underglow.h>
#include <zmk/split/bluetooth/peripheral.h>

#include <zmk/split/central.h>

#include <zephyr/logging/log.h>

#include <zmk_rgbled_widget/widget.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#ifndef ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_BATTERY
#define ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_BATTERY 0
#define ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_LAYER 1
#define ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_CONNECTIVITY 2
#endif

BUILD_ASSERT(!(SHOW_LAYER_CHANGE && SHOW_LAYER_COLORS),
             "CONFIG_RGBLED_WIDGET_SHOW_LAYER_CHANGE and CONFIG_RGBLED_WIDGET_SHOW_LAYER_COLORS "
             "are mutually exclusive");

// map from color values to names, for logging
static const char *color_names[] = {"black", "red",     "green", "yellow",
                                    "blue",  "magenta", "cyan",  "white"};

static const struct zmk_led_hsb color_hsb[] = {
    {0, 0, 0},       // black
    {0, 100, 100},   // red
    {120, 100, 100}, // green
    {60, 100, 100},  // yellow
    {240, 100, 100}, // blue
    {300, 100, 100}, // magenta
    {180, 100, 100}, // cyan
    {0, 0, 100},     // white
};

#define STATUS_PIXELS_MAX 5

#if SHOW_LAYER_COLORS
static const uint8_t layer_color_idx[] = {
    CONFIG_RGBLED_WIDGET_LAYER_0_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_1_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_2_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_3_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_4_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_5_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_6_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_7_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_8_COLOR,  CONFIG_RGBLED_WIDGET_LAYER_9_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_10_COLOR, CONFIG_RGBLED_WIDGET_LAYER_11_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_12_COLOR, CONFIG_RGBLED_WIDGET_LAYER_13_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_14_COLOR, CONFIG_RGBLED_WIDGET_LAYER_15_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_16_COLOR, CONFIG_RGBLED_WIDGET_LAYER_17_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_18_COLOR, CONFIG_RGBLED_WIDGET_LAYER_19_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_20_COLOR, CONFIG_RGBLED_WIDGET_LAYER_21_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_22_COLOR, CONFIG_RGBLED_WIDGET_LAYER_23_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_24_COLOR, CONFIG_RGBLED_WIDGET_LAYER_25_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_26_COLOR, CONFIG_RGBLED_WIDGET_LAYER_27_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_28_COLOR, CONFIG_RGBLED_WIDGET_LAYER_29_COLOR,
    CONFIG_RGBLED_WIDGET_LAYER_30_COLOR, CONFIG_RGBLED_WIDGET_LAYER_31_COLOR,
};
#endif

// log shorthands
#define LOG_CONN_CENTRAL(index, status, color_label)                                               \
    LOG_INF("Profile %d %s, blinking %s", index, status,                                           \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_CONN_PERIPHERAL(status, color_label)                                                   \
    LOG_INF("Peripheral %s, blinking %s", status,                                                  \
            color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_##color_label])
#define LOG_BATTERY(battery_level, color_label)                                                    \
    LOG_INF("Battery level %d, blinking %s", battery_level,                                        \
            color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_##color_label])

// a blink work item as specified by the color and duration
struct blink_item {
    uint8_t color;
    uint32_t duration_ms;
    uint32_t sleep_ms;
    uint16_t status_pixel_index;
    uint16_t status_pixel_indices[STATUS_PIXELS_MAX];
    uint8_t status_pixel_colors[STATUS_PIXELS_MAX];
    uint8_t status_pixel_count;
    uint8_t status_pixel_blink_index;
    uint8_t status_channel;
    bool blink;
    bool blink_until_connected;
    bool blink_status_pixel;
    bool use_status_pixel;
    bool use_status_pixels;
};

// flag to indicate whether the initial boot up sequence is complete
static bool initialized = false;

struct underglow_state {
    bool on;
    struct zmk_led_hsb color;
    int effect;
};

static int save_underglow_state(struct underglow_state *state) {
    int ret = zmk_rgb_underglow_get_state(&state->on);
    if (ret < 0) {
        return ret;
    }

    state->color = zmk_rgb_underglow_calc_brt(0);
    state->effect = zmk_rgb_underglow_calc_effect(0);

    return 0;
}

static void restore_underglow_state(const struct underglow_state *state, uint32_t duration_ms) {
    zmk_rgb_underglow_select_effect(state->effect);
    zmk_rgb_underglow_set_hsb(state->color);

    if (state->on) {
        zmk_rgb_underglow_on();
    } else {
        zmk_rgb_underglow_off();
    }

    if (duration_ms > 0) {
        k_sleep(K_MSEC(duration_ms));
    }
}

__attribute__((weak)) int zmk_rgb_underglow_status_pixel(uint16_t index,
                                                         struct zmk_led_hsb color) {
    ARG_UNUSED(index);
    ARG_UNUSED(color);

    return -ENOSYS;
}

__attribute__((weak)) int zmk_rgb_underglow_clear_status_pixel(void) { return -ENOSYS; }

__attribute__((weak)) int zmk_rgb_underglow_clear_status_channel(uint8_t channel) {
    ARG_UNUSED(channel);

    return -ENOSYS;
}

__attribute__((weak)) int zmk_rgb_underglow_status_channel_pixels(
    uint8_t channel, const uint16_t *indices, const struct zmk_led_hsb *colors, uint8_t len) {
    ARG_UNUSED(channel);
    ARG_UNUSED(indices);
    ARG_UNUSED(colors);
    ARG_UNUSED(len);

    return -ENOSYS;
}

__attribute__((weak)) int zmk_rgb_underglow_status_pixels(const uint16_t *indices,
                                                          const struct zmk_led_hsb *colors,
                                                          uint8_t len) {
    ARG_UNUSED(indices);
    ARG_UNUSED(colors);
    ARG_UNUSED(len);

    return -ENOSYS;
}

// low-level method to control the underglow strip
static void set_rgb_leds(uint8_t color, uint32_t duration_ms) {
    color &= 0x07;

    if (color == 0) {
        zmk_rgb_underglow_off();
    } else {
        zmk_rgb_underglow_select_effect(0);
        zmk_rgb_underglow_set_hsb(color_hsb[color]);
        zmk_rgb_underglow_on();
    }

    if (duration_ms > 0) {
        k_sleep(K_MSEC(duration_ms));
    }

}

static bool set_status_pixel_led(uint8_t channel, uint16_t index, uint8_t color,
                                 uint32_t duration_ms) {
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL)
    static bool warned_unavailable;

    color &= 0x07;

    int ret = zmk_rgb_underglow_status_channel_pixels(channel, &index, &color_hsb[color], 1);
    if (ret == 0) {
        if (duration_ms > 0) {
            k_sleep(K_MSEC(duration_ms));
        }

        return true;
    }

    if (!warned_unavailable) {
        LOG_WRN("Single-pixel connectivity indicator unavailable (%d), using full underglow", ret);
        warned_unavailable = true;
    }
#endif

    return false;
}

static bool set_status_pixels_led(const struct blink_item *blink, uint8_t color,
                                  uint32_t duration_ms) {
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS) || SHOW_LAYER_CHANGE
    static bool warned_unavailable;
    struct zmk_led_hsb colors[STATUS_PIXELS_MAX];
    uint8_t count = MIN(blink->status_pixel_count, (uint8_t)STATUS_PIXELS_MAX);

    if (count == 0) {
        return false;
    }

    for (uint8_t i = 0; i < count; i++) {
        uint8_t pixel_color = blink->status_pixel_colors[i];

        if (blink->blink_status_pixel && color == 0 && i == blink->status_pixel_blink_index) {
            pixel_color = 0;
        }

        colors[i] = color_hsb[pixel_color & 0x07];
    }

    int ret =
        zmk_rgb_underglow_status_channel_pixels(blink->status_channel, blink->status_pixel_indices,
                                                colors, count);
    if (ret == 0) {
        if (duration_ms > 0) {
            k_sleep(K_MSEC(duration_ms));
        }

        return true;
    }

    if (!warned_unavailable) {
        LOG_WRN("Multi-pixel battery indicator unavailable (%d), using full underglow", ret);
        warned_unavailable = true;
    }
#endif

    return false;
}

static void set_indicator_leds(const struct blink_item *blink, uint8_t color,
                               uint32_t duration_ms) {
    if (blink->use_status_pixels && set_status_pixels_led(blink, color, duration_ms)) {
        return;
    }

    if (blink->use_status_pixel &&
        set_status_pixel_led(blink->status_channel, blink->status_pixel_index, color,
                             duration_ms)) {
        return;
    }

    set_rgb_leds(color, duration_ms);
}

static bool key_position_to_status_pixel(uint16_t position, uint16_t *pixel) {
    if (pixel == NULL) {
        return false;
    }

#if IS_ENABLED(CONFIG_ZMK_SPLIT)
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    if (position <= 5) {
        *pixel = 5 - position;
        return true;
    }

    if (position >= 12 && position <= 17) {
        *pixel = position - 6;
        return true;
    }

    if (position >= 24 && position <= 29) {
        *pixel = position - 12;
        return true;
    }

    if (position >= 36 && position <= 39) {
        *pixel = position - 18;
        return true;
    }
#else
    if (position >= 6 && position <= 11) {
        *pixel = position - 6;
        return true;
    }

    if (position >= 18 && position <= 23) {
        *pixel = position - 12;
        return true;
    }

    if (position >= 30 && position <= 35) {
        *pixel = position - 18;
        return true;
    }

    if (position >= 40 && position <= 43) {
        *pixel = position - 22;
        return true;
    }
#endif
#else
    *pixel = position;
    return true;
#endif

    return false;
}

static uint16_t get_ble_profile_status_pixel(uint8_t profile_index) {
    static const uint16_t profile_status_pixels[] = {
        CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_0,
        CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_1,
        CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_2,
        CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_3,
        CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_PROFILE_4,
    };

#if IS_ENABLED(CONFIG_ZMK_BLE) && DT_NODE_EXISTS(DT_NODELABEL(bt))
    for (zmk_keymap_layer_index_t layer_index = 0; layer_index < ZMK_KEYMAP_LAYERS_LEN;
         layer_index++) {
        zmk_keymap_layer_id_t layer_id = zmk_keymap_layer_index_to_id(layer_index);

        if (layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }

        for (uint16_t position = 0; position < ZMK_KEYMAP_LEN; position++) {
            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(layer_id, position);

            if (binding == NULL || binding->behavior_dev == NULL) {
                continue;
            }

            if (strcmp(binding->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(bt))) == 0 &&
                binding->param1 == BT_SEL_CMD && binding->param2 == profile_index) {
                uint16_t pixel;

                if (key_position_to_status_pixel(position, &pixel)) {
                    return pixel;
                }
            }
        }
    }
#endif

    return profile_index < ARRAY_SIZE(profile_status_pixels)
               ? profile_status_pixels[profile_index]
               : CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX;
}

static bool should_continue_advertising_blink(void) {
#if (!IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)) &&                 \
    IS_ENABLED(CONFIG_ZMK_BLE)
    return zmk_ble_active_profile_is_open() && !zmk_ble_active_profile_is_connected();
#else
    return false;
#endif
}

// define message queue of blink work items, that will be processed by a
// separate thread
K_MSGQ_DEFINE(led_msgq, sizeof(struct blink_item), 16, 1);

static bool last_connectivity_valid = false;
static struct blink_item last_connectivity;
static struct blink_item active_connectivity;
static struct k_work_delayable connectivity_status_work;
static bool connectivity_status_active;
static bool connectivity_status_lit;
static int64_t connectivity_status_until;

static bool connectivity_matches_last(const struct blink_item *blink) {
    return last_connectivity_valid && last_connectivity.color == blink->color &&
           last_connectivity.blink == blink->blink &&
           last_connectivity.blink_until_connected == blink->blink_until_connected &&
           last_connectivity.use_status_pixel == blink->use_status_pixel &&
           last_connectivity.status_pixel_index == blink->status_pixel_index &&
           last_connectivity.status_channel == blink->status_channel;
}

static void clear_status_channel(uint8_t channel) {
    int ret = zmk_rgb_underglow_clear_status_channel(channel);

    if (ret == -ENOSYS) {
        zmk_rgb_underglow_clear_status_pixel();
    }
}

static void connectivity_status_cb(struct k_work *work) {
    ARG_UNUSED(work);

    if (!connectivity_status_active) {
        return;
    }

    if (active_connectivity.blink_until_connected) {
        if (!should_continue_advertising_blink()) {
            clear_status_channel(active_connectivity.status_channel);
            connectivity_status_active = false;
            connectivity_status_lit = false;
            return;
        }

        connectivity_status_lit = !connectivity_status_lit;
        set_status_pixel_led(active_connectivity.status_channel, active_connectivity.status_pixel_index,
                             connectivity_status_lit ? active_connectivity.color : 0, 0);
        k_work_reschedule(&connectivity_status_work,
                          K_MSEC(CONFIG_RGBLED_WIDGET_INTERVAL_MS));
        return;
    }

    if (active_connectivity.blink) {
        if (k_uptime_get() >= connectivity_status_until) {
            clear_status_channel(active_connectivity.status_channel);
            connectivity_status_active = false;
            connectivity_status_lit = false;
            return;
        }

        connectivity_status_lit = !connectivity_status_lit;
        set_status_pixel_led(active_connectivity.status_channel, active_connectivity.status_pixel_index,
                             connectivity_status_lit ? active_connectivity.color : 0, 0);
        k_work_reschedule(&connectivity_status_work,
                          K_MSEC(CONFIG_RGBLED_WIDGET_INTERVAL_MS));
        return;
    }

    if (!connectivity_status_lit) {
        set_status_pixel_led(active_connectivity.status_channel, active_connectivity.status_pixel_index,
                             active_connectivity.color, 0);
        connectivity_status_lit = true;
        k_work_reschedule(&connectivity_status_work, K_MSEC(active_connectivity.duration_ms));
    } else {
        clear_status_channel(active_connectivity.status_channel);
        connectivity_status_active = false;
        connectivity_status_lit = false;
    }
}

static void start_connectivity_status(const struct blink_item *blink) {
    active_connectivity = *blink;
    connectivity_status_active = true;
    connectivity_status_lit = false;
    connectivity_status_until = k_uptime_get() + blink->duration_ms;
    k_work_reschedule(&connectivity_status_work, K_NO_WAIT);
}

static void stop_solid_connectivity_status(void) {
    if (!connectivity_status_active || active_connectivity.blink ||
        active_connectivity.blink_until_connected) {
        return;
    }

    k_work_cancel_delayable(&connectivity_status_work);
    clear_status_channel(active_connectivity.status_channel);
    connectivity_status_active = false;
    connectivity_status_lit = false;
}

static void indicate_connectivity_internal(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_CONN_BLINK_MS,
                               .status_pixel_index = CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL_INDEX,
                               .status_channel = ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_CONNECTIVITY,
                               .use_status_pixel =
                                   IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL)};

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#if IS_ENABLED(CONFIG_ZMK_BLE)
    uint8_t profile_index = zmk_ble_active_profile_index();
#endif

    switch (zmk_endpoint_get_selected().transport) {
    case ZMK_TRANSPORT_USB: // USB connected and selected
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
        LOG_INF("USB connected, blinking %s", color_names[CONFIG_RGBLED_WIDGET_CONN_COLOR_USB]);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_USB;
        break;
#endif
    case ZMK_TRANSPORT_BLE: // BLE connected and selected
#if IS_ENABLED(CONFIG_ZMK_BLE)
        LOG_CONN_CENTRAL(profile_index, "connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
        blink.status_pixel_index = get_ble_profile_status_pixel(profile_index);
        break;
#endif
    default: // ZMK_TRANSPORT_NONE, neither BLE nor USB connected
#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (zmk_endpoint_get_preferred_transport() != ZMK_TRANSPORT_NONE &&
            zmk_ble_active_profile_is_open()) {
            LOG_CONN_CENTRAL(profile_index, "open", ADVERTISING);
            blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_ADVERTISING;
            blink.status_pixel_index = get_ble_profile_status_pixel(profile_index);
            blink.blink = true;
            blink.blink_until_connected = true;
            break;
        }
#endif
        LOG_CONN_CENTRAL(-1, "no endpoints connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
#if IS_ENABLED(CONFIG_ZMK_BLE)
        blink.status_pixel_index = get_ble_profile_status_pixel(profile_index);
#endif
        blink.blink = true;
        break;
    }
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
    if (zmk_split_bt_peripheral_is_connected()) {
        LOG_CONN_PERIPHERAL("connected", CONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_CONNECTED;
    } else {
        LOG_CONN_PERIPHERAL("not connected", DISCONNECTED);
        blink.color = CONFIG_RGBLED_WIDGET_CONN_COLOR_DISCONNECTED;
        blink.blink = true;
    }
#endif

    if (connectivity_matches_last(&blink)) {
        LOG_DBG("Skipping duplicate connectivity indication");
        return;
    }

    if (blink.use_status_pixel) {
        start_connectivity_status(&blink);
        last_connectivity = blink;
        last_connectivity_valid = true;
    } else if (k_msgq_put(&led_msgq, &blink, K_NO_WAIT) == 0) {
        last_connectivity = blink;
        last_connectivity_valid = true;
    }
}

// debouncing to ignore all but last connectivity event, to prevent repeat blinks
static struct k_work_delayable indicate_connectivity_work;
static void indicate_connectivity_cb(struct k_work *work) { indicate_connectivity_internal(); }
void indicate_connectivity() { k_work_reschedule(&indicate_connectivity_work, K_MSEC(16)); }

static int led_output_listener_cb(const zmk_event_t *eh) {
    if (initialized) {
        indicate_connectivity();
    }
    return 0;
}

ZMK_LISTENER(led_output_listener, led_output_listener_cb);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
// run led_output_listener_cb on endpoint and BLE profile change (on central)
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_SHOW_USB)
ZMK_SUBSCRIPTION(led_output_listener, zmk_endpoint_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(led_output_listener, zmk_ble_active_profile_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BLE)
#elif IS_ENABLED(CONFIG_ZMK_SPLIT_BLE)
// run led_output_listener_cb on peripheral status change event
ZMK_SUBSCRIPTION(led_output_listener, zmk_split_peripheral_status_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)
static inline uint8_t get_battery_color(uint8_t battery_level) {
    if (battery_level == 0) {
        LOG_INF("Battery level undetermined (zero), blinking %s",
                color_names[CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING]);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MISSING;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_HIGH) {
        LOG_BATTERY(battery_level, HIGH);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_HIGH;
    }
    if (battery_level >= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_LOW) {
        LOG_BATTERY(battery_level, MEDIUM);
        return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_MEDIUM;
    }
    LOG_BATTERY(battery_level, LOW);
    return CONFIG_RGBLED_WIDGET_BATTERY_COLOR_LOW;
}

static void configure_battery_status_pixels(struct blink_item *blink, uint8_t battery_level) {
#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS)
    static const uint16_t battery_status_pixels[] = {
        CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_0,
        CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_1,
        CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_2,
        CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_3,
        CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXEL_4,
    };

    uint8_t filled_segments = battery_level == 0 ? ARRAY_SIZE(battery_status_pixels)
                                                 : CLAMP((battery_level + 19) / 20, 1, 5);

    blink->use_status_pixels = true;
    blink->status_channel = ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_BATTERY;
    blink->status_pixel_count = ARRAY_SIZE(battery_status_pixels);
    blink->blink = true;
    blink->blink_status_pixel = true;
    blink->status_pixel_blink_index = filled_segments - 1;

    for (uint8_t i = 0; i < ARRAY_SIZE(battery_status_pixels); i++) {
        blink->status_pixel_indices[i] = battery_status_pixels[i];
        blink->status_pixel_colors[i] = i < filled_segments ? blink->color : 0;
    }
#else
    ARG_UNUSED(blink);
    ARG_UNUSED(battery_level);
#endif
}

static bool binding_enables_layer(const struct zmk_behavior_binding *binding,
                                  zmk_keymap_layer_id_t layer_id) {
    if (binding == NULL || binding->behavior_dev == NULL || binding->param1 != layer_id) {
        return false;
    }

#if DT_NODE_EXISTS(DT_NODELABEL(mo))
    if (strcmp(binding->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(mo))) == 0) {
        return true;
    }
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(tog))
    if (strcmp(binding->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(tog))) == 0) {
        return true;
    }
#endif

#if DT_NODE_EXISTS(DT_NODELABEL(to))
    if (strcmp(binding->behavior_dev, DEVICE_DT_NAME(DT_NODELABEL(to))) == 0) {
        return true;
    }
#endif

    return false;
}

static bool configure_layer_status_pixels(struct blink_item *blink,
                                          zmk_keymap_layer_index_t layer_index) {
#if SHOW_LAYER_CHANGE
    zmk_keymap_layer_id_t target_layer_id = zmk_keymap_layer_index_to_id(layer_index);

    if (target_layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
        return false;
    }

    blink->use_status_pixels = true;
    blink->status_channel = ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_LAYER;
    blink->status_pixel_count = 0;

    for (zmk_keymap_layer_index_t scan_layer_index = 0; scan_layer_index < ZMK_KEYMAP_LAYERS_LEN;
         scan_layer_index++) {
        zmk_keymap_layer_id_t scan_layer_id = zmk_keymap_layer_index_to_id(scan_layer_index);

        if (scan_layer_id == ZMK_KEYMAP_LAYER_ID_INVAL) {
            continue;
        }

        for (uint16_t position = 0; position < ZMK_KEYMAP_LEN; position++) {
            const struct zmk_behavior_binding *binding =
                zmk_keymap_get_layer_binding_at_idx(scan_layer_id, position);

            if (!binding_enables_layer(binding, target_layer_id)) {
                continue;
            }

            uint16_t pixel;

            if (!key_position_to_status_pixel(position, &pixel)) {
                continue;
            }

            blink->status_pixel_indices[blink->status_pixel_count] = pixel;
            blink->status_pixel_colors[blink->status_pixel_count] = blink->color;
            blink->status_pixel_count++;

            if (blink->status_pixel_count >= STATUS_PIXELS_MAX) {
                return true;
            }
        }
    }

    return blink->status_pixel_count > 0;
#else
    ARG_UNUSED(blink);
    ARG_UNUSED(layer_index);
    return false;
#endif
}

void indicate_battery(void) {
    struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS};
    int retry = 0;
    bool has_battery_level = false;
    uint8_t battery_level_to_show = 0;

    stop_solid_connectivity_status();

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_SELF) ||                                          \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS)
    uint8_t battery_level = zmk_battery_state_of_charge();
    while (battery_level == 0 && retry++ < 10) {
        k_sleep(K_MSEC(100));
        battery_level = zmk_battery_state_of_charge();
    };

    battery_level_to_show = battery_level;
    has_battery_level = true;
#endif

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_PERIPHERALS) ||                                   \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_SHOW_ONLY_PERIPHERALS)
    for (uint8_t i = 0; i < ZMK_SPLIT_BLE_PERIPHERAL_COUNT; i++) {
        uint8_t peripheral_level;
        int ret = zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
        if (ret == 0) {
            retry = 0;
            while (peripheral_level == 0 && retry++ < (CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS +
                                                       CONFIG_RGBLED_WIDGET_INTERVAL_MS) /
                                                          100) {
                k_sleep(K_MSEC(100));
                zmk_split_central_get_peripheral_battery_level(i, &peripheral_level);
            }

            if (peripheral_level == 0) {
                LOG_INF("Skipping undetermined battery level for peripheral %d", i);
                continue;
            }

            LOG_INF("Got battery level for peripheral %d:", i);
            if (!has_battery_level ||
                (battery_level_to_show != 0 && peripheral_level < battery_level_to_show)) {
                battery_level_to_show = peripheral_level;
            }
            has_battery_level = true;
        } else {
            LOG_ERR("Error looking up battery level for peripheral %d", i);
        }
    }
#endif

    if (has_battery_level) {
        blink.color = get_battery_color(battery_level_to_show);
        configure_battery_status_pixels(&blink, battery_level_to_show);
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
}

static int led_battery_listener_cb(const zmk_event_t *eh) {
    if (!initialized) {
        return 0;
    }

    // check if we are in critical battery levels at state change, blink if we are
    uint8_t battery_level = as_zmk_battery_state_changed(eh)->state_of_charge;

    if (battery_level > 0 && battery_level <= CONFIG_RGBLED_WIDGET_BATTERY_LEVEL_CRITICAL) {
        LOG_BATTERY(battery_level, CRITICAL);

        struct blink_item blink = {.duration_ms = CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS,
                                   .color = CONFIG_RGBLED_WIDGET_BATTERY_COLOR_CRITICAL};
        configure_battery_status_pixels(&blink, battery_level);
        k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
    }
    return 0;
}

// run led_battery_listener_cb on battery state change event
ZMK_LISTENER(led_battery_listener, led_battery_listener_cb);
ZMK_SUBSCRIPTION(led_battery_listener, zmk_battery_state_changed);
#endif // IS_ENABLED(CONFIG_ZMK_BATTERY_REPORTING)

uint8_t led_layer_color = 0;
#if SHOW_LAYER_COLORS
void update_layer_color(void) {
    uint8_t index = zmk_keymap_highest_layer_active();

    if (led_layer_color != layer_color_idx[index]) {
        led_layer_color = layer_color_idx[index];
        struct blink_item color = {.color = led_layer_color};
        LOG_INF("Setting layer color to %s for layer %d", color_names[led_layer_color], index);
        k_msgq_put(&led_msgq, &color, K_NO_WAIT);
    }
}

static int led_layer_color_listener_cb(const zmk_event_t *eh) {
    struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    // check if this is indeed an activity state changed event
    if (ev != NULL) {
        switch (ev->state) {
        case ZMK_ACTIVITY_SLEEP:
            LOG_INF("Detected sleep activity state, turn off LED");
            set_rgb_leds(0, 0);
            break;
        default: // not handling IDLE and ACTIVE yet
            break;
        }
        return 0;
    }

    // it must be a layer change event instead
    if (initialized) {
        update_layer_color();
    }
    return 0;
}

// run layer_color_listener_cb on layer status change event and activity state event
ZMK_LISTENER(led_layer_color_listener, led_layer_color_listener_cb);
ZMK_SUBSCRIPTION(led_layer_color_listener, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(led_layer_color_listener, zmk_activity_state_changed);
#endif // SHOW_LAYER_COLORS

#if !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
void indicate_layer(void) {
    uint8_t index = zmk_keymap_highest_layer_active();
    struct blink_item layer_indicator = {.duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
                                         .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
                                         .sleep_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS};

    if (configure_layer_status_pixels(&layer_indicator, index)) {
        LOG_INF("Showing %d layer key(s) for layer %d in %s", layer_indicator.status_pixel_count,
                index, color_names[CONFIG_RGBLED_WIDGET_LAYER_COLOR]);
        k_msgq_put(&led_msgq, &layer_indicator, K_NO_WAIT);
        return;
    }

    static const struct blink_item blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
        .sleep_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .status_channel = ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_LAYER,
    };
    static const struct blink_item last_blink = {
        .duration_ms = CONFIG_RGBLED_WIDGET_LAYER_BLINK_MS,
        .color = CONFIG_RGBLED_WIDGET_LAYER_COLOR,
        .status_channel = ZMK_RGB_UNDERGLOW_STATUS_CHANNEL_LAYER,
    };

    LOG_INF("No layer key found, blinking %d times %s for layer change", index,
            color_names[CONFIG_RGBLED_WIDGET_LAYER_COLOR]);

    for (int i = 0; i < index; i++) {
        if (i < index - 1) {
            k_msgq_put(&led_msgq, &blink, K_NO_WAIT);
        } else {
            k_msgq_put(&led_msgq, &last_blink, K_NO_WAIT);
        }
    }
}
#endif // !IS_ENABLED(CONFIG_ZMK_SPLIT) || IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#if SHOW_LAYER_CHANGE
static struct k_work_delayable layer_indicate_work;

static int led_layer_listener_cb(const zmk_event_t *eh) {
    // ignore if not initialized yet or layer off events
    if (initialized && as_zmk_layer_state_changed(eh)->state) {
        k_work_reschedule(&layer_indicate_work, K_MSEC(CONFIG_RGBLED_WIDGET_LAYER_DEBOUNCE_MS));
    }
    return 0;
}

static void indicate_layer_cb(struct k_work *work) { indicate_layer(); }

ZMK_LISTENER(led_layer_listener, led_layer_listener_cb);
ZMK_SUBSCRIPTION(led_layer_listener, zmk_layer_state_changed);
#endif // SHOW_LAYER_CHANGE

extern void led_process_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

    k_work_init_delayable(&indicate_connectivity_work, indicate_connectivity_cb);
    k_work_init_delayable(&connectivity_status_work, connectivity_status_cb);

#if SHOW_LAYER_CHANGE
    k_work_init_delayable(&layer_indicate_work, indicate_layer_cb);
#endif

    while (true) {
        // wait until a blink item is received and process it
        struct blink_item blink;
        k_msgq_get(&led_msgq, &blink, K_FOREVER);
        if (blink.duration_ms > 0) {
            LOG_DBG("Got a blink item from msgq, color %d, duration %d", blink.color,
                    blink.duration_ms);

            // Status blinks temporarily borrow the underglow strip. Snapshot the user-visible
            // state so battery/connectivity indicators do not leave their color behind.
            struct underglow_state saved_state;
            bool has_saved_state = save_underglow_state(&saved_state) == 0;
            uint32_t restore_ms =
                blink.sleep_ms > 0 ? blink.sleep_ms : CONFIG_RGBLED_WIDGET_INTERVAL_MS;

            // Show one solid status color, then restore the user-visible underglow state.
            if (blink.blink_until_connected) {
                while (should_continue_advertising_blink()) {
                    set_indicator_leds(&blink, blink.color, CONFIG_RGBLED_WIDGET_INTERVAL_MS);

                    if (!should_continue_advertising_blink()) {
                        break;
                    }

                    set_indicator_leds(&blink, 0, CONFIG_RGBLED_WIDGET_INTERVAL_MS);
                }
            } else if (blink.blink) {
                uint32_t elapsed_ms = 0;
                while (elapsed_ms < blink.duration_ms) {
                    uint32_t step_ms = MIN(CONFIG_RGBLED_WIDGET_INTERVAL_MS,
                                           blink.duration_ms - elapsed_ms);
                    set_indicator_leds(&blink, blink.color, step_ms);
                    elapsed_ms += step_ms;

                    if (elapsed_ms >= blink.duration_ms) {
                        break;
                    }

                    step_ms = MIN(CONFIG_RGBLED_WIDGET_INTERVAL_MS, blink.duration_ms - elapsed_ms);
                    set_indicator_leds(&blink, 0, step_ms);
                    elapsed_ms += step_ms;
                }
            } else {
                set_indicator_leds(&blink, blink.color, blink.duration_ms);
            }

            // Restore the borrowed underglow state before waiting for the next queued indicator.
            if (has_saved_state) {
                restore_underglow_state(&saved_state, 0);
            } else {
                set_rgb_leds(0, 0);
            }

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_CONN_STATUS_PIXEL) ||                                          \
    IS_ENABLED(CONFIG_RGBLED_WIDGET_BATTERY_STATUS_PIXELS) || SHOW_LAYER_CHANGE
            if (blink.use_status_pixel || blink.use_status_pixels) {
                clear_status_channel(blink.status_channel);
            }
#endif

            k_sleep(K_MSEC(restore_ms));

        } else {
            LOG_DBG("Got a layer color item from msgq, color %d", blink.color);
            set_rgb_leds(blink.color, 0);
        }
    }
}

// define led_process_thread with stack size 1024, start running it 100 ms after
// boot
K_THREAD_DEFINE(led_process_tid, 1024, led_process_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 100);

extern void led_init_thread(void *d0, void *d1, void *d2) {
    ARG_UNUSED(d0);
    ARG_UNUSED(d1);
    ARG_UNUSED(d2);

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BOOT_SHOW_BATTERY)
    // check and indicate battery level on thread start
    LOG_INF("Indicating initial battery status");

    indicate_battery();

#if !IS_ENABLED(CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY)
    // wait until blink should be displayed for further checks
    k_sleep(K_MSEC(CONFIG_RGBLED_WIDGET_BATTERY_BLINK_MS + CONFIG_RGBLED_WIDGET_INTERVAL_MS));
#endif
#endif // IS_ENABLED(CONFIG_RGBLED_WIDGET_BOOT_SHOW_BATTERY)

#if IS_ENABLED(CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY)
    // check and indicate current profile or peripheral connectivity status
    LOG_INF("Indicating initial connectivity status");
    indicate_connectivity();
#endif // IS_ENABLED(CONFIG_RGBLED_WIDGET_BOOT_SHOW_CONNECTIVITY)

#if SHOW_LAYER_COLORS
    LOG_INF("Setting initial layer color");
    update_layer_color();
#endif // SHOW_LAYER_COLORS

    initialized = true;
    LOG_INF("Finished initializing LED widget");
}

// run init thread on boot for initial battery+output checks
K_THREAD_DEFINE(led_init_tid, 1024, led_init_thread, NULL, NULL, NULL,
                K_LOWEST_APPLICATION_THREAD_PRIO, 0, 200);
