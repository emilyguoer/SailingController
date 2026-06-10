from pathlib import Path

Import("env")


def patch_ble_gamepad_nus(*args, **kwargs):
    project_dir = Path(env.subst("$PROJECT_DIR"))
    pio_env = env.subst("$PIOENV")
    ble_nus = (
        project_dir
        / ".pio"
        / "libdeps"
        / pio_env
        / "ESP32-BLE-Gamepad"
        / "BleNUS.cpp"
    )

    if not ble_nus.exists():
        return

    content = ble_nus.read_text(encoding="utf-8")
    if "#include <Arduino.h>" in content:
        return

    ble_nus.write_text("#include <Arduino.h>\n" + content, encoding="utf-8")
    print("Patched ESP32-BLE-Gamepad BleNUS.cpp for Arduino symbols")


patch_ble_gamepad_nus()
