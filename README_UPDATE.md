# PorkChop CYD — Update Notes

**Original project:** [M5PORKCHOP](https://github.com/0ct0sec/M5PORKCHOP) by **0ct0sec** — the ASCII piglet, the core WiFi security toolkit (handshake/PMKID capture, deauth, wardriving, recon), and the whole personality this project is built on all trace back to their original work. This update is a port and expansion of that project for the ESP32-2432S028R "Cheap Yellow Display" board.

**This update:** put together by **Xom**.

---

## What this update is

I took the CYD port and pushed it a lot further — a real visual identity for the idle screen, a proper bitmap sprite system replacing the old ASCII pig, a bunch of quality-of-life and reliability fixes, and some new toys. Below is what everything actually does, organized by area rather than the order I built it in, since a lot of this went through several iterations before landing where it did.

---

## The pig itself

The pig went from plain ASCII text (`(o 00)`-style characters) to a real hand-designed bitmap sprite:

- **7 mood states** (neutral, happy, excited, hunting, sleepy, sad, angry), each with left- and right-facing versions — 14 sprites total, packed as 2-bit-per-pixel data to keep flash usage small (~6KB)
- The sprite is genuinely **opaque** where it's drawn and genuinely **transparent** everywhere else, unlike the old font-based rendering — this is what makes it correctly read as "in front of" the background no matter what's behind it, instead of showing gaps or looking like a flat cutout
- **Blink** and **sniff** animations both work — sniff specifically flares one nostril at a time in the same `oo → oO → Oo` pattern the original ASCII version used
- The same sprite system now draws **everywhere** the pig appears — the main idle screen, the low-memory fallback rendering path, and the Party Pig easter egg's dancing pig. Nothing in the project still uses the old text-based pig.
- I also built a proper **cat**, **chicken**, and upgraded **butterfly**/**bird** sprites using the same technique, replacing what used to be single-line text emoticons

## The idle screen background — "FarmScene"

The idle screen isn't a blank background anymore. It's a single unified scene (`FarmScene`) that now shows on **every theme** — earlier in development I had it split between two different scene types depending on which color theme was active, but consolidated it into one so the whole experience feels consistent regardless of which theme you pick.

What's actually in it:
- **A barn** with a peaked roof, hayloft door, and main door
- **A windmill** with genuinely spinning blades (continuous rotation, not a fixed animation loop)
- **A tractor** with a small exhaust puff that rises and fades on a repeat cycle, so it reads as idling rather than parked
- **A scarecrow**, **hay bales**, **a mud puddle**, and **a fence line**
- **A distant cow silhouette**, deliberately drawn lighter/hazier than everything else — standard atmospheric depth cue for "far away"
- **Rolling pasture hills** along the horizon instead of jagged mountains — each hill has its own shade so they read as layered rather than one flat lump
- **A sun/moon day-night cycle** tied to how long the device has been running (there's no real-time clock on this hardware, so this is an honest uptime-based approximation, not a claim about actual time of day). First hour of the 2-hour cycle is a rising sun; second hour is a pale moon with a few small craters, followed by **twinkling stars** scattered across the sky, each blinking on its own independent timer
- Every object casts a **flat shadow** at its base (barn, windmill, tractor, cow) — small thing, but it's what stops the whole scene from looking like paper cutouts with nothing grounding them

Every element in the scene got its own shade rather than one flat silhouette color for everything — there's a genuine two-tier system where the hills stay subtle/light and the standing structures (barn, windmill, fence, scarecrow, tractor) stay clearly darker, with a guaranteed gap between the two tiers so nothing visually blends into whatever's behind it.

## Ambient life

- **Firefly** — a fourth critter type, meanders instead of crossing straight, pulses brightness instead of using a sprite (it's meant to read as a point of light)
- **Chicken** — pecks near the barn rather than crossing the screen like the other critters
- **Ambient oinks and moos** — occasional, unprompted, on independent random timers. Not tied to any gesture, just the farm feeling lived-in
- **Mud bath** — a hidden gesture (`DOWN DOWN LEFT RIGHT DOWN` on the menu screen) that sends the pig into a temporary muddy wallow: mud spots on its body and a side-to-side shake animation for a few seconds

## Theme system

- Cut down from 15 themes to a curated **8**: green/black, red/black, white/black, amber/black, pink/black, black/white, black/green, and yellow/blue. Fewer combinations to maintain, still covers the classic terminal looks.
- Found and fixed a real, non-obvious bug in the process: several themes have their foreground color *darker* than their background (the reverse of most themes). Every "make this darker" or "make this glow brighter" effect in the project had assumed foreground was always the bright one — on the flipped themes, glows were rendering backwards (darkest in the center instead of brightest) and shadows were rendering as barely-there instead of dark. Fixed with two small helper functions that check which color is actually darker/brighter before blending, rather than assuming.

## Feedback and celebration effects

- **Capture success** (handshake or PMKID): an expanding ring flashes from screen center, plus the onboard RGB LED flashes green
- **Deauth sent**: a brief red pulse at the screen edges, plus a red LED flash — throttled to at most once every 400ms, since deauth can fire in rapid bursts and flashing on every single packet would be more annoying than useful
- **Achievement unlocked**: the biggest celebration in the project — a 14-point sparkle burst in rotating rainbow colors, plus a fast RGB LED rainbow cycle. Achievements are rarer than routine captures, so they get the most elaborate treatment
- **Level up**: a physics-based confetti burst (this one existed before this update, kept as-is)

## Screen rotation

Tap the top-right corner from any screen to flip the display 180° — useful depending on how the device is mounted. The choice persists across reboots.

## Capture export

Handshakes and PMKIDs now also save as standard **.pcap** files alongside the existing hashcat-format exports, so captures can be opened directly in Wireshark, not just fed to hashcat. No real-time clock on this hardware means pcap timestamps are relative to boot rather than actual calendar time — an honest limitation, not a bug.

## UI consistency pass

A lot of small screens got touched to feel like one coherent app instead of features bolted on over time:
- Consistent rounded corners and drop shadows on every popup/card, with a theme-aware shadow color (plain black shadows are invisible on the several pure-black-background themes — this checks and picks a visible dark gray instead when needed)
- A shared ambient "circuit pulse" background (faint traveling data-pulse lines) on every list-style screen — Menu, Achievements, Settings, Diagnostics
- A consistent gesture language — top-left corner reliably means "back" across every screen that has a fast-exit option, rather than it being hold-only on some screens and tap-corner on others
- **Settings** got a real "cancel without saving" option for the first time — previously the only way out was holding, which always committed changes
- **Unlockables** reskinned as **PigDex** — numbered entries, matching header treatment, same underlying puzzle mechanic (the hint text needed to solve each one is still shown — I almost hid it Pokedex-style before catching that these are actual puzzle clues, not just flavor text)

## Reliability fixes found along the way

A few real bugs turned up while working on other things and got fixed rather than left alone:
- A buffer overflow in the handshake capture path — a stale size check left over from an earlier memory-saving change could overflow into adjacent struct fields on longer captured frames
- A heap-growth regression in an earlier version of the pcap feature — raw frame data was being kept for an entire session instead of only as long as actually needed, which matters on hardware this memory-constrained
- SD card mount reliability (clearer failure diagnostics, confirmed folder creation rather than trusting a blind `mkdir`)
- CPU/SD/network performance tuning — running the CPU at its full rated 240MHz explicitly rather than relying on IDE defaults, faster SD card I/O, faster WebUI polling now that the actual blocking-risk root cause is fixed rather than just worked around

---

*Everything above is on top of 0ct0sec's original M5PORKCHOP — the concept, the security toolkit itself, and the pig all started there. This document just covers what changed in this pass.*
