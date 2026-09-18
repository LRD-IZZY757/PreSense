# Presense

Occupancy-aware room automation, built for a hardware hackathon.

Presense uses three PIR motion sensors and a doorway-crossing algorithm to track whether a room is actually occupied, not just whether something moved, and automatically controls lighting and ventilation based on presence, activity, and sleep state. No app, no wearable, no camera. Just infrared sensors and some careful timing logic.

Simulate it live on Wokwi: https://wokwi.com/projects/471748998106519553

---

## The idea

Most "smart" occupancy switches are basically timers. Motion resets a countdown, and the lights turn off after N minutes of stillness. That fails in an obvious way: if you're sitting still reading, or asleep, the lights and fan will shut off on you.

Presense instead tracks people crossing the doorway, keeps a live occupant count, and moves the room through a state machine. That way "someone is here," "the lights should be on," and "the fan should keep running because someone's asleep" are treated as three separate things, not one.

---

## How it works

### 1. Doorway direction sensing (entry vs exit)
Two PIR sensors sit at the doorway, one facing outward (exterior), one facing inward (interior). Instead of just checking if both fired, Presense timestamps which one rose first:

- Exterior sensor rises first: someone is entering, occupant count +1
- Interior sensor rises first: someone is exiting, occupant count -1
- Both rise within about 15 ms: ambiguous, the cones overlapped too closely to tell, logged but not counted
- Only one sensor rises and the pair never completes within 4 seconds: partial crossing, someone approached the doorway and turned back

This is more forgiving than most doorway-counting builds since it tolerates overlapping sensor cones and the long output-hold times HC-SR501 sensors are known for.

### 2. Interior activity sensing
A third PIR sensor watches the interior of the room. Motion here resets the "last activity" timer, which drives the sleep countdown. The doorway sensors only handle who's in or out.

### 3. Room state machine
```
EMPTY -> ACTIVE -> WARNING -> SLEEPING -> EXITING -> EMPTY
```

| State | Meaning | Lights | Fan |
|---|---|---|---|
| EMPTY | No occupants | Off | Off |
| ACTIVE | Occupied, recent motion | On | On |
| WARNING | No motion for a while, about to sleep | Blinking | On |
| SLEEPING | Room considered asleep | Off | On (sleep-essential) |
| EXITING | Last person just left, grace period | Off | On (brief tail) |

The fan is treated as sleep-essential and stays on through SLEEPING and EXITING, while lights cut immediately. That's really the whole point of the SLEEPING state.

### 4. Safety net
If the occupant count is stuck above zero but there has been no interior or doorway motion for an extended stale period, the system assumes a missed exit and resets the count to zero, instead of leaving a room "occupied" forever because of a sensor miss.

### 5. Manual overrides
- Mode button: cycles AUTO -> FORCE ON -> FORCE OFF -> AUTO
- Snooze button: temporarily suspends the sleep and stale timers, freezing the room in ACTIVE, for a configurable duration

### 6. PC bridge (serial)
Presense streams telemetry over USB serial every 500 ms and accepts single-character commands back:

Outgoing (`$` prefixed CSV):
```
$occupants,room,light,fan,snooze,mode,secs,entries,exits,ambiguous,partial
```

Incoming commands:

| Char | Action |
|---|---|
| M | Cycle mode (Auto / Force On / Force Off) |
| S | Toggle snooze |
| R | Reset occupant count |

This is meant to pair with a small companion script (for example `presense.py`) for a dashboard or logging on a PC. Add yours to the repo if you build one.

---

## Hardware

| Component | Qty | Notes |
|---|---|---|
| Arduino Uno | 1 | Main controller |
| HC-SR501 PIR motion sensor | 3 | 2 at doorway (in/out), 1 interior |
| Relay module | 2 | Channel 1 = light, Channel 2 = fan |
| 16x2 I2C LCD (PCF8574, addr 0x27) | 1 | Status display |
| Push button | 2 | Mode select, snooze |
| LED | 1 | Status indicator |

### Pinout

| Pin | Function |
|---|---|
| D2 | PIR, entrance exterior |
| D3 | PIR, entrance interior |
| D4 | PIR, room interior |
| D5 | Relay, fan |
| D6 | LED, status indicator |
| D7 | Relay, light |
| D8 | Button, mode |
| D9 | Button, snooze |
| A4 / A5 | I2C LCD (SDA / SCL) |

---

## Configuration

Timing is controlled by a `DEMO_MODE` flag at the top of the sketch:

- `DEMO_MODE 1` (default): compressed timers for live demoing, sleep after 30s idle, exit grace of 20s, etc.
- `DEMO_MODE 0`: realistic timing for actual use, 20 min to sleep, 40 min stale timeout, 1 hour snooze.

Flip the flag and re-upload depending on whether you're demoing or deploying.

---

## Try it without hardware

The full circuit is simulated on Wokwi, no physical components required to see it running:
https://wokwi.com/projects/471748998106519553

---

## Built at [Hackathon Name]

Presense was built during a hardware hackathon as an exploration into presence-based, rather than timer-based, smart home automation, focused on doorway-crossing detection using only basic PIR sensors.

---

## License

MIT
