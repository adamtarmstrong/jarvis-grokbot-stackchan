# Set up Jarvis

This is the path from a new StackChan to a robot that talks to your own Grok Bot. Use your own Cloudflare account, xAI key, and webhook. Nothing in this repository is a credential.

There are no photos or a demo video in this version. No board was attached while the tree was written, so a screenshot would be invented.

## 1. What you need

- An M5Stack StackChan K151, or a CoreS3 seated in the Stack-chan base, and a USB-C data cable.
- A computer with ESP-IDF 5.5.4 (or Docker) and Node.js 22.
- An xAI API account, separate from a Grok subscription, with a monthly spending limit and a key used only for this robot.
- A new Cloudflare account used only for Jarvis. The free Worker plan may be enough for one device. Durable Objects sometimes require Workers Paid ($5). Confirm that on the deploy error if the free plan refuses the migration. This was not verified here.
- A Grok Bot account that can create a webhook routine.

## 2. Create the Jarvis bot

Create a bot named Jarvis. Add a webhook routine and paste `bot/WEBHOOK_ROUTINE.md` into it.

From the routine panel, copy the webhook URL and the bearer key into a password manager. The URL looks like `https://api2.cursor.sh/automations/webhook/<id>`. That host is the product's webhook host. Do not commit the id or the key.

The routine answers in one to three spoken sentences, or it hands only the question text to the Jarvis bot so that bot can relay. The final answer is posted back to `reply_url` with the one-time `reply_token`.

## 3. Deploy the Worker

From a clone of this repository:

```sh
cd worker
npm install
npx wrangler login
```

Log in with the Jarvis Cloudflare account. Then put the secrets. The script hashes the device token before it is stored. It writes `worker/secrets.generated` (mode 0600) so you can copy the device token into the robot later. It does not print secret values.

```sh
export XAI_API_KEY='the xAI key'
export JARVIS_WEBHOOK_KEY='the routine bearer key'
export JARVIS_WEBHOOK_URL='https://api2.cursor.sh/automations/webhook/THE_ID'
export PUBLIC_BASE_URL='https://the-worker-host-you-were-assigned'
# Optional. Leave unset and the script generates them.
# export DEVICE_TOKEN=...
# export DEVICE_HMAC_KEY=...
# export DEVICE_ID=...
# export PATH_PREFIX=...
./scripts/secrets-setup.sh
npx wrangler deploy
```

`PUBLIC_BASE_URL` is the `https://` origin Wrangler prints after deploy, with no path. You may have to deploy once, read that host, set `PUBLIC_BASE_URL`, and deploy again. The origin is not a secret in the same way a key is, but this tree still does not hard-code one.

`wrangler.jsonc` has no account id. Do not add one.

### Worker secrets

| Secret | What it is |
| --- | --- |
| `XAI_API_KEY` | xAI key. Stays in the Worker. The robot never sees it. |
| `JARVIS_WEBHOOK_KEY` | Bearer token for the Grok Bot webhook. |
| `JARVIS_WEBHOOK_URL` | Full webhook URL, `https` only. |
| `DEVICE_TOKEN_HASH` | Hex SHA-256 of the device token. The raw token lives in the robot's NVS and in your password manager. |
| `DEVICE_HMAC_KEY` | Shared HMAC secret. The same string is typed into the robot. It is a UTF-8 string, not hex-decoded. |
| `DEVICE_ID` | The one device id this Worker accepts. |
| `PATH_PREFIX` | Random path segment, at least 8 characters, no slashes. |
| `PUBLIC_BASE_URL` | `https://<worker-host>` with no path. Used to build `reply_url`. |

Plain variables in `wrangler.jsonc` are only the caps: 60 session minutes per day, 6 token grants per hour, a 600 second client secret, a 300 second command timeout, 2000 characters, a 30 second duplicate window, voice `leo`, model `grok-voice-3.0`.

Check the deploy locally before you point a robot at it:

```sh
cd worker
npm test
npx tsc --noEmit
```

## 4. Flash the firmware

A browser flasher is not in this version. With ESP-IDF 5.5.4:

```sh
cd firmware
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

`idf.py set-target` writes `sdkconfig`, which is gitignored. Commit `sdkconfig.defaults` only.

The GitHub Action `.github/workflows/firmware.yml` builds with `espressif/idf:v5.5.4` and runs the static RAM check. See `firmware/docs/UNTESTED.md` if that image tag is missing.

## 5. First-run setup

On a chip with empty NVS the firmware opens a setup access point named `Jarvis-Setup` (open, no password) and a page at `http://192.168.4.1/`. It also starts BLE provisioning under the name `Jarvis`. The serial log prints a one-boot proof-of-possession for BLE. That line is a setup PIN, not a cloud key.

The page asks for the Wi-Fi name and password, the Worker URL (`PUBLIC_BASE_URL`), the path prefix, the device id, the device token (the raw token, not the hash), and the HMAC key. Those values are stored in NVS. They are not compiled in.

Leave the access point only for the minutes you need. It is an open network.

## 6. Voice and wake word

The voice is `leo`. The only other voice this Worker will return is `altair`, and only if you set the plain variable `XAI_VOICE` to `altair` before deploy.

The wake word is the esp-sr preset `wn9_jarvis_tts`. Hold-to-talk is the Si12T middle or back pad, or a finger on the screen. Both are untested on hardware.

The robot asks xAI for small talk. Longer questions go to `POST /ask`, and the bot's reply comes back on the WebSocket and is read aloud.

## 7. Optional pieces that are not in this version

Other bots are reached only by the handoff described in `bot/WEBHOOK_ROUTINE.md`. There is no custom MCP connector and no face tracking. The camera driver is compiled out, and the camera privacy dot stays off.

## 8. Troubleshooting

- Boot loop or immediate abort in Wi-Fi init: the static RAM check failed or was skipped. esp-sr 2.5.4 did this on CoreS3. Stay on `~2.6.0`. Read `firmware/docs/known_issues.md`.
- The head hits a stop: the software range is yaw ±40° and pitch −10° to +25° around zeros 460 and 620. The mechanical tilt is about 5–85°. Calibrate the zero on the assembled base. Torque drops when the head is still.
- The robot reboots after 10–40 minutes while the LEDs and the speaker are both busy: the M5GFX I2C lock described in the known-issues note. Not fixed here.
- Worker returns an empty 401: the clock is off by more than 60 seconds, the nonce was reused, the token or HMAC does not match, or the device id does not match. The body is empty on purpose.
- Worker returns an empty 404: the path prefix is wrong, or the path is not one of `/token`, `/ask`, `/bot/reply`, `/ws`.
- Webhook 401 from the bot platform: the `JARVIS_WEBHOOK_KEY` does not match the routine. A 401 from `POST /bot/reply` means the `reply_token` was wrong, reused, or older than five minutes.
- Empty 429: the daily minute cap, the hourly token cap, the one active voice session, or the rate-limit binding (30 requests per 60 seconds per IP and per device).

## 9. Costs

xAI voice is about $0.08 per minute while a realtime session is open. Five minutes a day is about $12 a month, fifteen about $36, thirty about $72. The Worker charges the daily cap for the whole client-secret window (up to 10 minutes) when it grants a token, and releases that charge if xAI fails. Whether xAI itself bills idle time inside the window was not measured.

Cloudflare is $0 if the free plan accepts Durable Objects, or $5 a month for Workers Paid. Grok Bot routine runs come out of that subscription. How much one run consumes is not published here.

Set a monthly spending limit on the xAI key before the first session.

## 10. Security

- Cloudflare: a new account, a unique password, a passkey or hardware key for 2FA, and login notifications. No other members.
- API tokens: never the Global API Key. Create a custom token that can edit Workers Scripts, edit Workers KV storage, and read account settings. Add an expiry date. Limit the token to your IP if you deploy from one place. Durable Objects may ask for one more permission at deploy time. Add only what that error names.
- Keep the token in a password manager. `secrets.generated` is gitignored. Delete it when the robot has the device token.
- Rotate with `worker/scripts/rotate-all.sh --yes`. That replaces the device token, HMAC key, path prefix, and device id, and it overwrites the xAI key and the webhook key with values that will not work, so the old ones stop being used by the Worker. You then revoke the xAI key in the xAI console, rotate the webhook key in the bot panel, and run `secrets-setup.sh` again with the new values. The robot stays disconnected until you enter the new device token, HMAC key, prefix, and id in the setup page.
- `--dry-run` lists the secret names and changes nothing.
- Turn on GitHub secret scanning and push protection in the repository settings. The files here cannot switch that on.
- Logs from the Worker are request id, route, status, and milliseconds. They do not include tokens, keys, or transcripts.
