import requests
import time
import json

BASE_URL = "http://192.168.4.1"

def pretty(data):
    print(json.dumps(data, indent=2))


def get_state():
    r = requests.get(f"{BASE_URL}/api/state", timeout=2)
    r.raise_for_status()
    return r.json()


def test_state():
    print("\n=== GET /api/state ===")
    try:
        data = get_state()
        pretty(data)
        return data
    except Exception as e:
        print("State request failed:", e)


def test_arm_cycle():
    print("\n=== POST /api/arm (toggle test) ===")
    try:
        before = get_state()["state"]
        print("Initial state:", before)

        requests.post(f"{BASE_URL}/api/arm", timeout=2)
        time.sleep(0.5)
        mid = get_state()["state"]
        print("After 1st toggle:", mid)

        requests.post(f"{BASE_URL}/api/arm", timeout=2)
        time.sleep(0.5)
        after = get_state()["state"]
        print("After 2nd toggle:", after)

    except Exception as e:
        print("Arm test failed:", e)


def test_settings_safe():
    print("\n=== POST /api/settings (safe config) ===")

    try:
        current = get_state()

        payload = {
            "interval": current.get("intervalSec", 5),
            "focusEnabled": 1 if current.get("focusEnabled", True) else 0,
            "focusMs": current.get("focusMs", 200),
            "preFocus": 1 if current.get("preFocus", True) else 0,
            "shutterMs": current.get("shutterMs", 150),
            "pinsSwapped": 1 if current.get("pinsSwapped", False) else 0,
            "pinsActiveLow": 1 if current.get("pinsActiveLow", False) else 0,
            # IMPORTANT: always keep WiFi ON
            "wifiEnabled": 1
        }

        r = requests.post(
            f"{BASE_URL}/api/settings",
            data=payload,
            timeout=2
        )
        r.raise_for_status()

        print("Settings applied (WiFi kept ON)")

        time.sleep(0.5)
        updated = get_state()
        pretty(updated)

    except Exception as e:
        print("Settings test failed:", e)


def run_all_tests():
    test_state()
    test_arm_cycle()
    test_settings_safe()


if __name__ == "__main__":
    run_all_tests()