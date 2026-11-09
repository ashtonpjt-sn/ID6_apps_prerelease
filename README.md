# NBT Evo ID6 apps

## Gallery

<table>
  <tr>
    <td><img src="images/01_boostdeck_daylight.jpg" width="300"></td>
    <td><img src="images/02_boostdeck_closeup.jpg" width="300"></td>
  </tr>
  <tr>
    <td><img src="images/05_shiftdeck.jpg" width="300"></td>
    <td><img src="images/06_boostdeck_night.jpg" width="300"></td>
  </tr>
  <tr>
    <td><img src="images/07_boostdeck_ambient.jpg" width="300"></td>
    <td><img src="images/08_mhd_maps.jpg" width="300"></td>
  </tr>
</table>

## New in this fork

Three more apps under **My Vehicle**, plus two upgrades. None has been tested on a car yet.

- **Warm-Up**: oil and coolant temperature, a rev limit that rises as the engine warms, and time to working temperature
- **Performance Timer**: automatic 0–60 mph, ¼ mile, 60–100 mph, 60–130 mph and 60–0 mph braking, with best results kept
- **Fault Scanner**: reads fault codes from every control unit. It can also clear them, after two presses and only once the DME confirms the car is stopped
- **BoostDeck → OPTIONS → More vitals**: oil temperature, low-pressure fuel, battery, fuel trims, ethanol, engine load and misfires per cylinder
- **ShiftDeck**: a gear indicator that learns your car's ratios

Details and safeguards: [source/docs/NEW_APPS.md](source/docs/NEW_APPS.md)

## Coming Soon (In Development)

<table>
  <tr>
    <td><img src="images/03_hud_white.jpg" width="300"></td>
    <td><img src="images/04_hud_orange.jpg" width="300"></td>
  </tr>
</table>


This repo contains all of the source files and instructions needed for an agentic ai to compile gauges/menus for your specific resolution and version of idrive, please simply link this repo to an ai that can control your pc such as Codex and let the magic happen!
I would personally recommend GPT Sol 6.1 with High reasoning because it is fully capable of doing all this and still gives you a good amount of usage time.
By the way, all of these menus have extra options, for example if you are in the boost/afr menu, you can press the "Options" button on your idrive controller to access color changes for the gauges! 
Have fun and if you come across any issues, message b58devkev on Instagram, also tag the account if you get it working!! Make sure to share with friends, this is free after all!

Park your car, connect it to your computer with ENET, and hand this repository to an AI agent that can run commands on that computer. The agent uses [AgentReadMe.md](AgentReadMe.md) to identify your head unit, check compatibility, and install BoostDeck, ShiftDeck, and MHD Maps under **My Vehicle** when the checks pass.

## Start with your car and an agent

1. Park the car, switch on the ignition, and wait for iDrive to start. Connect the car's OBD port to the computer with a wired ENET cable. Use a USB Ethernet adapter if needed.
2. Open an AI agent with terminal access on that computer, such as Codex running locally. Give it the prompt below. The agent needs access to the computer plugged into the car.
3. Stay with the car. The agent may need you to enable temporary SSH with the unlock tool, read the firmware version from iDrive, or verify the SSH fingerprint. Keep iDrive awake and ENET connected through the install. Restart iDrive only after the agent confirms the installed files were verified, then check the menus while parked.

### Copy this into your agent

> My car is parked, iDrive is awake, and this computer is connected to the car over wired ENET. Read https://github.com/inotbarnacle/ID6_apps_prerelease/blob/main/AgentReadMe.md and follow it on this computer to install BoostDeck, ShiftDeck, and MHD Maps. Identify my head unit and check its firmware, framework hashes, and active display resolution. Use the local installer automation when the checks pass. If the firmware is unlisted or the frameworks differ, start with the read-only compatibility capture and follow the porting workflow. Tell me when you need me at the car. Have me enter any SSH password locally. After installation, tell me when to restart iDrive and what to verify.

## What the agent checks

The published installer has profiles for `NBTevo_N18092I`, `NBTevo_V20153I`, and `NBTevo_Y24073J`. The agent checks the exact firmware and both framework hashes before writing files. An unlisted build needs compatibility work and parked validation before it can be treated as supported. [Unlisted firmware workflow](AgentReadMe.md#bring-up-an-unlisted-firmware)

BoostDeck's BMW-specific gauge definitions come from a 2016 340i and may vary by ECU. ShiftDeck uses standard OBD RPM and speed. MHD Maps requires a compatible MHD+ multi-map tune. Check the active slot in MHD separately if you test map switching. [Signal and map details](source/README.md)


