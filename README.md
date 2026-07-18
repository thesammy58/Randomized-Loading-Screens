# Randomized-Loading-Screens
ASI mod for The Sims 3 that randomizes your loading screen before each startup. Designed to require as little configuration and brain power from the player as I could possibly manage. Just drag-and-drop the ASI file and MyLoadingScreens folder to your Program Files\EA Games\The Sims 3\Game\Bin directory. Requires an ASI loader to work.

# What it'll do as soon as you launch the game:

-Read from a pool of available loading screen packages in Bin\MyLoadingScreens (comes preloaded with all packs, also supports custom loading screens)

-Detect your locale based on the game's registry entries, with multiple detection fallbacks in place if registry read fails

-Copy randomly selected loading screen package to a new folder called "LoadingScreen" located inside your mods folder. It will be called active_loadingscreen.package

-On first run, add a new entry to resource.cfg ensuring this package takes precedence over all other potential loading screen UI edits (it will first save a back up of your current resource.cfg to the LoadingScreen folder, just in case)

-Writes to a file called last_pick.txt (also in the LoadingScreen folder) to help ensure that the same loading screen is never randomly selected twice in a row

-Check for a Tiny UI Fix installation (more on this below)

-Save each action to a log file to help with troubleshooting if something doesn't work for you

# Tiny UI Fix stuff

In order to make my loading screens still scale with a user's chosen Tiny UI Fix setting, a workaround had to be put in place. The ASI will check to see if a Tiny UI Fix package exists in the Mods/TinyUIFix directory. If one does, the ASI will harvest the randomly chosen loading screen's matching native LAYO (native meanting that SPECIFIC expansion/stuff pack's layout resource ID) from Tiny UI Fix, and inject that scaled LAYO in its entirety back in to our chosen loading screen. Since my loading screens each have 21 identical LAYO entries (so that the loading screen looks the same regardless of the user's latest installed pack), each package file has been stamped with a different marker resource, identifying to the ASI exactly which specific pack's LAYO should be harvested from Tiny UI Fix and injected back into all 21 of our LAYOs.

# Localizations

Several game localizations generate a user-data folder in a language other than "The Sims 3". In order to place the chosen loading screen (and its log file, resource.cfg edit, etc) in to the correct Sims 3 folder, a detection hierarchy was put in place in case one method fails. It is ordered as such:

1. An optional .ini file specifying folder language (downloaded by the user if detection methods fail)
2. The game's registry entry
3. The user's operating system locale
4. Simply assume the folder is titled "The Sims 3"
