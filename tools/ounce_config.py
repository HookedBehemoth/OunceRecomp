"""The user's ounce config (~/.config/ounce/config.toml, the file `ounce config` shows) as the Python tools read it:
each game's dump and the data folder. A tool imports it with its folder's parent on sys.path."""
import os
import sys
import tomllib


def path():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.join(os.path.expanduser("~"), ".config")
    return os.path.join(base, "ounce", "config.toml")


def load():
    try:
        with open(path(), "rb") as f:
            return tomllib.load(f)
    except FileNotFoundError:
        return {}


def dump(game):
    """The game's dump folder (exefs/ and romfs/); stops with one line naming the fix when there is none."""
    d = load().get("games", {}).get(game, {}).get("dump")
    if not d:
        sys.exit("no dump for %s: run `ounce config set %s.dump <folder with exefs/ and romfs/>`" % (game, game))
    d = os.path.expanduser(d)
    if not os.path.isdir(os.path.join(d, "exefs")):
        sys.exit("the dump for %s (%s, from %s) has no exefs/ folder" % (game, d, path()))
    return d


def data_dir():
    """The data folder: the saves, and per game scenes/, golden/ and rec/."""
    d = load().get("paths", {}).get("data")
    if d:
        return os.path.expanduser(d)
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "ounce")
