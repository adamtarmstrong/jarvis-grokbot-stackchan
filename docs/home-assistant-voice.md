# Home Assistant voice setup (first slice)

See [the add-on installation guide](../homeassistant/jarvis-bridge/README.md) for
exact options, protocol, secrets handling and current limitations.

1. Once published, add the project's GitHub **repository URL** to Home Assistant
   OS → Settings → Add-ons → Add-on store → Repositories. Install Jarvis Bridge.
   This is not installed through HACS.
2. Configure the existing Worker's URL **including its prefix**, device identity
   and secret signing credentials in the add-on; set a matching LAN pairing key.
   The Worker continues to send the webhook and accept `/bot/reply`. No inbound
   callback tunnel is needed in this recommended mode.
3. Start and wait for models-ready in the log. Public users automatically get
   official Piper `en_GB-alan-medium` from the official upstream voice repository.
   Nothing needs to be heard at the Pi; all recording/playback is on the robot.
4. Only after separately flashing reviewed firmware, hold the robot's display at
   boot to reopen its setup page. Enter `ws://PI-IP:10700/voice` plus the pairing
   key and Wi-Fi. Save/reboot. No Mac or phone app is required for normal operation.
5. Private voice: put your own authorized `NAME.onnx` and `NAME.onnx.json` in
   `/share/piper`, set `custom_voice: NAME`, restart. These files stay private,
   are never downloaded by us, and are never bundled or sold. Clear the setting
   for Alan. See the add-on's license/provenance notice before bundling any weights.

Say Hey Jarvis and talk immediately. The green microphone icon in the top-left
shows capture (including pre-connection buffering); the amber clock shows busy
or speaking. No heard-you/ready beeps in this HA path. Grok eyes, squinting,
privacy dots and the 2px white/last-15s-amber 60s turn bar remain.

**Not deployed yet:** only local code/build/tests exist. No push, HA installation,
robot flash, paid speech call or cloud webhook test was performed. Camera remains
off; follow/schedule/color form values are stored hooks only. In particular the
old idle motion is not yet disabled after hours. Future default is every day
08:00–17:00 America/Detroit, no movement outside hours, voice still available.
