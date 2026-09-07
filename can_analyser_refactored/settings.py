import configparser
import json
from pathlib import Path

_SETTINGS_PATH = Path(__file__).with_name("analyzer_settings.json")
_CONFIG_PATH = Path("config.ini")
_DEFAULT_SETTINGS = {
    "language": "PL",
    "filter_text": "",
    "bus_filter_index": 0,
    "delta_enabled": False,
    "id_filter_enabled": False,
    "update_existing_ids": False,
    "autoscroll_enabled": True,
    "active_tab_index": 0,
    "window_geometry": None,
}


def load_settings():
    settings = _DEFAULT_SETTINGS.copy()

    config = configparser.ConfigParser()
    config.read(_CONFIG_PATH, encoding="utf-8")
    if config.has_section("UI"):
        ui = config["UI"]
        settings["language"] = ui.get("language", settings["language"])
        settings["filter_text"] = ui.get("filter_text", settings["filter_text"])
        settings["bus_filter_index"] = ui.getint("bus_filter_index", fallback=settings["bus_filter_index"])
        settings["delta_enabled"] = ui.getboolean("delta_enabled", fallback=settings["delta_enabled"])
        settings["id_filter_enabled"] = ui.getboolean("id_filter_enabled", fallback=settings["id_filter_enabled"])
        settings["autoscroll_enabled"] = ui.getboolean("autoscroll_enabled", fallback=settings["autoscroll_enabled"])
        settings["active_tab_index"] = ui.getint("active_tab_index", fallback=settings["active_tab_index"])
        geometry = ui.get("window_geometry", fallback="")
        settings["window_geometry"] = geometry or None
        return settings

    # Migrate settings written by older versions that used JSON.
    try:
        with _SETTINGS_PATH.open("r", encoding="utf-8") as settings_file:
            loaded_settings = json.load(settings_file)
    except (OSError, json.JSONDecodeError):
        return settings

    if isinstance(loaded_settings, dict):
        for key in settings:
            if key in loaded_settings:
                settings[key] = loaded_settings[key]
    return settings


def save_settings(settings):
    try:
        config = configparser.ConfigParser()
        config.read(_CONFIG_PATH, encoding="utf-8")
        if not config.has_section("UI"):
            config.add_section("UI")

        for key, default in _DEFAULT_SETTINGS.items():
            value = settings.get(key, default)
            if value is None:
                value = ""
            config["UI"][key] = str(value)

        with _CONFIG_PATH.open("w", encoding="utf-8") as config_file:
            config.write(config_file)
    except OSError:
        return False
    return True
