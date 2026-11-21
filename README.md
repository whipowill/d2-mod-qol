# Will Scarlet's QOL for Diablo II

A mod for Diablo II introducing various quality-of-life features and bug fixes.

**Note this was designed for D2LOD v1.13 and will not work on version 1.14.**

## Features

All of these features are optional and toggled in the ``qol.ini`` file.

- **Auto Click** - hold spacebar for repeated left mouse clicks (works w/ PD2)
- **Auto Pickup** - potions, scrolls, keys, and gold (works w/ PD2)
- **Hitcap 100%** - attack rating can reach 100% hit chance on monsters
- **No XP Penalty** - no longer penalized for fighting low lvl monsters, or partying high level friends
- **Death Keep** - no longer lose your gear when you die
- **Merc Equip** - mercs can equip various item types consistent w/ their class, including quivers
- **Merc Drop** - mercs will drop their gear if you hire a new one
- **Merc Whirl** - mercs can use Whirlwind-style attacks properly, if they have that skill
- **Zoo Keeper** - the minions you had when you logged out will auto respawn
- **Wide Hallways** - Maggot Lair and Arcane Sanctuary now have wider hallways
- **Infinite Ammo** - quivers and throwing weapons no longer consume ammo
- **Achievement Aura** - bask in the glow of your achievement with Hell completion aura
- **Random DClone** - stay on your toes w/ randomized DClone spawns as you play
- **Terror Zones** - random zones will double gold, XP, MF, and monster density
- **Horde Mode** - increase the default monster density of the game

### Bug Fixes

- **Merc Run** - mercs run when following you instead of walking
- **NM FE Fix** - fire enchants explode w/ correct damage in Nightmare
- **CPU Fix** - fixes legacy CPU bug in the original game
- **NHAM Fix** - fixes legacy "next hit always misses" problem

## Commands

These commands only work if ``cheats`` are turned on in the config file:

- ``.god`` - make yourself invulnerable for fun
- ``.xp on|off`` - control when you gain XP if you want to
- ``.density 10`` - play custom monster density settings on the fly
- ``.reset skills|stats`` - reset your skills and stats on demand
- ``.dclone`` - force Diablo to walk the Earth

## Install

Copy and paste the files from ``build/`` into your game folder, and add the ``qol.dll`` to your game DLL loader.

## License

MIT, see `LICENSE`.

## External Links

- [Will Scarlet's PlugY Modpack](https://github.com/whipowill/d2-plugy-qol) - a compilation of all the best mods to play the game
- [Will Scarlet's QOL UI Mod](https://github.com/whipowill/d2-mod-qol-ui) - another DLL mod that provides maphack, loot filter, and more