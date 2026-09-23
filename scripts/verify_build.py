"""Check the generated firmware configuration, not just source defaults."""

from pathlib import Path
import re


def require_config(text, symbol, expected):
    match = re.search(rf"^{symbol}=(.*)$", text, re.M)
    actual = match.group(1) if match else "n"
    if actual != expected:
        raise SystemExit(f"{symbol}: expected {expected}, got {actual}")


def main():
    for half in ("L", "R"):
        directory = Path("build") / f"Cygnus_{half}" / "zephyr"
        config = (directory / ".config").read_text(encoding="utf-8")
        dts = (directory / "zephyr.dts").read_text(encoding="utf-8")
        matrix = re.search(r"\bkscan\s*\{([^}]+)\}", dts, re.S)
        if not matrix or "wakeup-source;" not in matrix.group(1):
            raise SystemExit(f"Cygnus_{half}: matrix is not a wakeup source")
        for symbol, expected in {
            "CONFIG_ZMK_SLEEP": "y",
            "CONFIG_ZMK_IDLE_TIMEOUT": "30000",
            "CONFIG_ZMK_IDLE_SLEEP_TIMEOUT": "1800000",
            "CONFIG_BT_CTLR_PHY_2M": "n",
            "CONFIG_BT_CTLR_TX_PWR_PLUS_8": "y",
            "CONFIG_ZMK_SETTINGS_RESET_ON_START": "n",
        }.items():
            require_config(config, symbol, expected)
        if half == "R":
            for symbol, expected in {
                "CONFIG_CYGNUS_INPUT_PROCESSOR_GESTURE": "y",
                "CONFIG_PMW3610_REPORT_INTERVAL_MIN": "12",
                "CONFIG_PMW3610_INIT_POWER_UP_EXTRA_DELAY_MS": "200",
                "CONFIG_PMW3610_INVERT_X": "n",
                "CONFIG_PMW3610_INVERT_Y": "y",
                "CONFIG_BT_PERIPHERAL_PREF_MIN_INT": "6",
                "CONFIG_BT_PERIPHERAL_PREF_MAX_INT": "12",
            }.items():
                require_config(config, symbol, expected)
        print(f"PASS: Cygnus_{half} wakeup, preserved connection defaults, no settings reset.")


if __name__ == "__main__":
    main()
