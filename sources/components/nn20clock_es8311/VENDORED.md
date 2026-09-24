# ES8311 driver, vendored

This is `espressif/es8311` version 1.0.0 from the ESP component registry,
copied in and modified. Apache-2.0; `LICENSE` is upstream's, unchanged.

## Why it is a copy and not a dependency

The published driver talks to the codec through `driver/i2c.h`, ESP-IDF's
legacy I2C API. On this board the codec shares I2C port 0 with the GT911
touch controller, and `nn20clock_display` brings that bus up with the
current API, `driver/i2c_master.h`.

The two cannot coexist. ESP-IDF 5.5 links a constructor into the legacy
driver that aborts at startup if the new one is present:

    components/driver/i2c/i2c.c:1731   check_i2c_driver_conflict()

So this is not a matter of preference or tidiness - depending on the
published component would make the firmware abort before `app_main`. The
registry has no version of this driver using the new API (checked:
1.0.0 is the newest).

The alternative was to move the display's touch back to the legacy API.
That was rejected: the legacy driver is deprecated, the ESP-IDF touch
and panel components are on the new one, and it would have made the
whole project follow this driver backwards.

## What was changed

Everything else - the clock coefficient table, the register sequences,
the init order - is upstream's and should stay that way. When a frame is
silent or distorted, this file is not where the bug is.

- `es8311.h`: includes `driver/i2c_master.h` instead of `driver/i2c.h`.
- `es8311_create()` takes an `i2c_master_bus_handle_t` rather than an
  `i2c_port_t`. The bus is borrowed, not created: the display owns it,
  and its lifetime is why `nn20clock_audio` is constructed after the
  display and destroyed before it.
- The device is added to the bus in `es8311_create()` at 100 kHz, the
  speed the tryout used.
- `es8311_write_reg` / `es8311_read_reg` use `i2c_master_transmit` and
  `i2c_master_transmit_receive`.
- `es8311_delete()` removes the device from the bus and tolerates NULL.
- `es8311_create()` no longer dereferences a failed `calloc`.

## Updating

If Espressif publishes a version using `driver/i2c_master.h`, delete this
component and depend on it instead. Diff against the registry copy first:
the modifications above are all confined to the I2C layer, so a re-port
is a small job.
