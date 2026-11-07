# QOL DLL for Diablo 2

A game mod for Diablo 2 introducing various quality-of-life features and bug fixes.

**Note this was designed for D2LOD v1.13 and will not work on version 1.14.**

## Features

- **Auto Click** - hold spacebar to generate repeated left mouse clicks
- **Auto Pickup** - potions, scrolls, and keys hop into your belt and bags (works w/ PD2)
- **Merc Run** - mercs run after you instead of walking (bug fix)
- **Merc Equip** - mercs can now equip various item types consistent w/ their class (including quivers)
- **Merc Drop** - mercs will now drop their gear if you hire a new one
- **No XP Penalty** - no longer penalizes you for fighting low lvl monsters, or partying high level friends
- **Death Keep** - no longer lose your gear when you die
- **Merc Whirlwind** - mercs can use Whirlwind-style attacks properly and carve through packs (for mod authors giving mercs the skill)
- **Druid Summons** - leave with your wolves, bear, vines and ravens out and they're back, free, when you load in

All of these features are optional and toggled in the ``qol.ini`` file.

## Install

Copy and paste the files from ``build/`` into your game folder, and add the ``qol.dll`` to your game DLL loader.

## License

MIT, see `LICENSE`.