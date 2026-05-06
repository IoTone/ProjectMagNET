# `craw_status_led`

Semantic wrapper around the kit's single WS2812 RGB LED. The rest of the
firmware sets a mode; this component handles the colour and animation.

## Modes

| Mode | Visual | When |
|---|---|---|
| `CRAW_LED_BOOTING` | Solid warm amber | First moments after power-on |
| `CRAW_LED_IDLE` | Slow soft cyan pulse (4 s period) | No presence detected |
| `CRAW_LED_PRESENCE` | 2 s pulse, hue from BPM | Person in cone — hue maps 50 BPM → blue-violet, 100 BPM → red |
| `CRAW_LED_ERROR` | 1 Hz red blink | Caller signals a fault |
| `CRAW_LED_OFF` | Off | — |

## API

```c
craw_status_led_init(CONFIG_CRAW_LED_GPIO);
craw_status_led_set_mode(CRAW_LED_BOOTING, 0);

/* in the main loop */
craw_status_led_tick(50);            // dt in ms
craw_status_led_set_mode(CRAW_LED_PRESENCE, (int)current_bpm);
```

`tick()` is the only animation driver — call every ~50 ms from the main
loop. There's no dedicated task.

## Why HSV → RGB

Driving the BPM-hue mode from HSV space keeps the saturation and value
fixed while only the hue varies, which reads as a clean colour shift on
the LED. RGB-space interpolation gives muddy mid-tones.
