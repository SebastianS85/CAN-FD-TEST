import json
from pathlib import Path

_SETTINGS_PATH = Path(__file__).with_name("analyzer_settings.json")
_DEFAULT_SETTINGS = {
    "language": "PL",
    "filter_text": "",
    "bus_filter_index": 0,
    "delta_enabled": False,
    "autoscroll_enabled": True,
    "active_tab_index": 0,
    "window_geometry": None,
}


def load_settings():
    settings = _DEFAULT_SETTINGS.copy()
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
    saved_settings = _DEFAULT_SETTINGS.copy()
    for key in saved_settings:
        if key in settings:
            saved_settings[key] = settings[key]

    try:
        with _SETTINGS_PATH.open("w", encoding="utf-8") as settings_file:
            json.dump(saved_settings, settings_file, indent=2)
            settings_file.write("\n")
    except OSError:
        return False
    return True
