from pathlib import Path


def greeting():
    return (Path(__file__).parent / "resources" / "greeting.txt").read_text(encoding="utf-8").strip()
