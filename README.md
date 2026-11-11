# CYD Spotify Display

A mini "now playing" screen for the **ESP32-2432S028R** (Cheap Yellow Display):
album cover, title / artist / album, a live progress bar, and touch buttons for
previous / play-pause / next.

```
+----------+-----------------+
|  album   | Title           |
|  cover   | Artist          |
|          | Album           |
+----------+-----------------+
| ======progress========     |
| 1:23                  3:45 |
|    |<      (>)      >|     |
+----------------------------+
```

## 1. Create a Spotify app (5 min)

1. Go to <https://developer.spotify.com/dashboard> and log in. **The account must have
   Spotify Premium**, which Spotify now requires for developer apps.
2. **Create app**
   - Name / description: anything
   - **Redirect URI:** `http://127.0.0.1:8888/callback` (type it exactly, then click *Add*)
   - Tick **Web API**, accept the terms, and click *Save*
3. Open the app's **Settings** and copy the **Client ID** and **Client secret**.

> Why 127.0.0.1? Spotify no longer accepts plain `http://` redirect addresses except
> `127.0.0.1`, so it can't redirect straight to the display's local IP. Instead, you
> copy the address from the browser once and paste it into the display's web page
> (step 4).

## 2. Set up the project

1. Install **VS Code** and the **PlatformIO IDE** extension.
2. *File -> Open Folder...* and pick this folder. PlatformIO downloads the ESP32
   toolchain and libraries the first time (takes a few minutes).
3. Copy `include/secrets.example.h` to `include/secrets.h` and fill in your
   WiFi details (**2.4 GHz only**, the ESP32 can't use 5 GHz) plus the Client ID and secret.

## 3. Flash it

Which board do you have?

| Board | Environment |
|---|---|
| One micro-USB port | `cyd` (default) |
| Two ports (micro-USB + USB-C) | `cyd2usb` |

Plug it in with a **data** USB cable, then click the PlatformIO **Upload** arrow
in the bottom bar (or run `pio run -e cyd -t upload`). Open the **Serial Monitor**
(plug icon) to see logs at 115200 baud.

- Port not found? Install the **CH340** USB driver.
- Upload stuck at "Connecting..."? Hold the **BOOT** button on the back while it connects.

## 4. Log in (once)

1. The screen shows *"Log in to Spotify"* and an address like `http://192.168.178.42`.
   Open it on your phone or PC (same WiFi).
2. Tap **Log in with Spotify**, then **Agree**.
3. The browser ends up on `http://127.0.0.1:8888/callback?code=...` and shows an
   error page. **That's expected.** Copy the **whole address** from the address bar.
4. Go back to the display's page, paste it, and press **Connect**. The code expires after
   a few minutes, so do this right away.

The login is saved in flash and survives reboots. Spotify makes you log in again
about every 6 months. When that happens the display shows the login screen
on its own, and you repeat step 4.

## Troubleshooting

| Problem | Fix |
|---|---|
| White / black screen | Wrong environment: try the other one (`cyd` / `cyd2usb`) |
| Colours look inverted (negative) | Switch environment: `cyd` <-> `cyd2usb` |
| Picture upside down | In `src/ui.cpp` use `tft.setRotation(3)`, and in `src/main.cpp` `ts.setRotation(3)` |
| Taps hit the wrong button | Tap the corners, read the `touch raw=` lines in the Serial Monitor, adjust `TOUCH_X_MIN/MAX`, `TOUCH_Y_MIN/MAX` in `main.cpp` |
| "No active device" when tapping | Spotify needs an open player: start playing on your phone/PC once |
| "That didn't work... INVALID_CLIENT" | The Redirect URI in the dashboard doesn't match `secrets.h` exactly |
| Accented letters | The built-in fonts are ASCII-only, so á/ñ/ü are shown as a/n/u (and ¿ ¡ as ? !) |

## How it works

- `src/spotify.cpp`: OAuth (code to refresh token, stored in flash with `Preferences`),
  polls `GET /me/player/currently-playing` (every 3 s while playing, 5 s paused,
  10 s idle, plus right when a song ends) and sends play/pause/next/previous.
  JSON is parsed with an ArduinoJson **filter**, so only the needed fields use RAM.
- `src/ui.cpp`: TFT_eSPI drawing. It downloads the 300 px cover and decodes it at 1/2 scale
  (150 px) with TJpg_Decoder, word-wraps text, and draws the progress bar without flicker.
- `src/main.cpp`: WiFi, the small login web server, touch handling, and the main loop.
  Between polls the progress bar is interpolated locally, so it moves smoothly.

## Ideas for next steps

- Run the network calls in a separate FreeRTOS task so taps never wait for a poll
- Volume slider (`PUT /me/player/volume?volume_percent=`)
- Dim the backlight (PWM on GPIO 21) after a while with nothing playing, and use the LDR on the board
- Background colour taken from the album cover
- Smooth (anti-aliased) Unicode fonts from LittleFS so accents render properly
