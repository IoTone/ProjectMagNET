# `craw_bh1750`

Minimal ESP-IDF driver for the Rohm BH1750 ambient light sensor over I²C.

- Address: 0x23 (default) — the kit uses this; alt 0x5C is unused here.
- Mode: continuous high-resolution (1 lux precision, ~120 ms conversion).
- Range: 1–65 535 lux.
- Bus: I²C0, 100 kHz, internal pull-ups enabled.

## API

```c
#include "craw_bh1750.h"

craw_bh1750_init(CONFIG_CRAW_BH1750_SDA_GPIO, CONFIG_CRAW_BH1750_SCL_GPIO);

float lux;
if (craw_bh1750_read(&lux) == ESP_OK) {
    printf("%.1f lux\n", lux);
}
```

## Notes

- Uses ESP-IDF v5+ `i2c_master.h` (the new driver), not the legacy `i2c.h`.
- The `init()` call seeds a 200 ms wait for the first conversion to complete;
  subsequent reads return promptly.
- If you share the I²C0 bus with another sensor later, refactor to expose
  the `i2c_master_bus_handle_t` rather than owning the bus internally.
