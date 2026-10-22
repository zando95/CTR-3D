# Changelog

What changed for players in each CTR 3D release, newest first. Add a line here in the same commit as any change a player would notice.

## Unreleased

- The track-select preview video plays at a steady 30 fps.
- The Scrapbook no longer crashes. It starts straight away instead of after a long black screen, shows the art in the right place, and plays at its full 15 fps.
- The boot screens are no longer shifted up with their top lines cut off.
- Saves are crash-safe. A save interrupted by a crash or power loss can no longer wipe your progress; the game falls back to the previous copy.
- A disc image that is damaged, incomplete, not the North American (NTSC-U) disc, or in the wrong format (.iso, .cue, compressed) is now caught at boot with a message saying what is wrong and what to do, instead of booting into broken levels.
- A disc read that fails mid-game ends on an error screen instead of hanging.
- If the game can't start, it shows a message instead of a black screen. An original 3DS, 3DS XL or 2DS gets "New Nintendo 3DS required".
- Fixed a timing bug that could make animated water and track textures glitch.
- The boot screens' small legal text is smoothed instead of breaking up.
- OPTIONS settings are remembered between launches.
- New AUTOSAVE GHOST option (on by default): your fastest Time Trial run on each track is saved automatically, and picking that track races you against it.
- The Super Turbo Pads cheat code (ARROW) now also turns on Retro-Fueled mode. Any boost that isn't a pad (power slides, hang time, turbo items, the start boost) no longer knocks you out of USF. Square+Down, or Square in the air, keeps your reserves, and without Cross held you really brake or reverse. Every pad is a blue pad, and each gives a 1-second reserve. The exhaust fire burns blue at USF speed and the menu flag turns blue. Races in this mode save no times or best laps and don't count as beating N. Tropy or N. Oxide. Enter ARROW again to turn it off.
- Saving a ghost over an existing one now stores the new run under its own file name. Before, the memory card kept the old run's name, so after a restart the ghost list showed the old run's character, track and time for the new tape.
- The main menu has a new last row, QUIT. Picking it asks YES or NO (NO is preselected), and YES exits the game the same way HOME > Close does, back to the Homebrew Launcher or the HOME Menu.
- New BOSS RACES row in the ARCADE race-type menu: race the boss of the track's hub (Ripper Roo, Papu Papu, Komodo Joe, Pinstripe, or Oxide on Oxide Station, Slide Coliseum and Turbo Track) on any track you have unlocked, three laps, with the boss's Adventure difficulty. It never changes your Adventure save.
- New CTR CHALLENGE row in the ARCADE race-type menu: the Adventure token race as an Arcade race. Pick a difficulty and a track (the 16 Adventure tracks), collect the letters C, T and R scattered on the track and finish first with all three. It never changes your Adventure save.
- New RELIC RACE row in the ARCADE race-type menu: the Adventure relic race on any track you have unlocked (all 18, with the time crates and the sapphire, gold and platinum times), three laps. Your five best times per track go into the memory card's relic high scores, the same table Adventure relic races use and the main menu's HIGH SCORE screen shows, through the usual NEW HIGH SCORE name entry and memory card save. The relics shown in this mode follow those times (Adventure relic times count too), and a relic won here never changes an Adventure profile. A cheat that can change a time records nothing.
- In RELIC RACE, the track-select list shows a spinning relic beside every track you have a relic on, tinted sapphire, gold or platinum for the best one earned (Adventure relic times count). Tracks with no relic show nothing.
- RELIC RACE counts a relic as earned if any Adventure profile on the memory card has it, not only the card-wide best times: the goal on the HUD, the relic awarded and the relics on track select all follow. The relics are rebuilt at every race start, so a relic that was never saved (for example a new record whose name entry was left unfinished) does not carry over into RETRY.
- CTR CHALLENGE winning is remembered: finishing first with all three letters on a track (any difficulty, no cheat that can change the race) is saved in a small file on the memory card, CTR3DTOKENS. The track-select list then shows a spinning CTR token, in the colour Adventure gives that track's token, beside every track whose token anyone on the save has earned: an Arcade win here, or any Adventure profile on the memory card. It is only a mark; no Adventure token or save is changed.

## beta 1.0 (2026-09-16)

First public release.
