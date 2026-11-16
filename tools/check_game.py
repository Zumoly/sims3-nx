#!/usr/bin/env python3
"""Check that a game folder is the one Sims3-NX was built for.

usage: python check_game.py <game folder> [update folder]

<game folder> is your extracted Xbox 360 disc (default.xex at its top), <update folder> the output of
extract_tu.py. Sims3-NX contains code translated from these exact executables, so any other version will not run.
Only the Python standard library is used.
"""
import hashlib
import os
import sys

GAME = {
    "default.xex": "51d76e0aa218a7b7476ada729ad7ab249af567b8289825ac32d7af3ca169a52a",
    "Sims3.xex": "492a3182c1694fd6b7ecd08579a29a874f10b8f4738737ac840a001bd178fdae",
    "PrivLoader.xex": "1c454f279912306af872a5d2e7ff1b61b09fc3fe6e0ecdaff7d76c72c7fdbfd4",
}
UPDATE = {
    "globalpatch.idx": "7e9ecb1955f9fd82f4b09b69eb0ec2cf8874b85f6b2c5fe8fa3a7c6af49ca92b",
    "Sims3.xexp": "e141effed399bdc4d6702c428cc5e7a65a53a16fd8584abbe8b872358de0a952",
    "PrivLoader.xexp": "39081ad344b5b22ed10e7cb432b3785a9c8dbabcca314d24a42ad404af6aa6f0",
    "GameplayRelease/gameplay.package": "452edc0373bd3e0cf8921633be5597d3d6bcd2ab81d9646bfdaf2e4388ba71f7",
    "GameplayRelease/scripts.package": "45668add593b2a014866badeb47d5b7d36c65a1320d02557623ae8015de904bd",
}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check(folder, expected):
    good = True
    for rel, digest in expected.items():
        path = os.path.join(folder, *rel.split("/"))
        if not os.path.isfile(path):
            print("  %-34s MISSING" % rel)
            good = False
            continue
        ok = sha256(path) == digest
        good &= ok
        print("  %-34s %s" % (rel, "ok" if ok else "DIFFERENT VERSION"))
    return good


def main():
    if len(sys.argv) not in (2, 3):
        print(__doc__)
        return 2
    print("Game (%s):" % sys.argv[1])
    good = check(sys.argv[1], GAME)
    for extra in ("GameplayRelease", "UI", "GameData"):
        if not os.path.isdir(os.path.join(sys.argv[1], extra)):
            print("  %-34s MISSING (copy the whole disc, not only the .xex files)" % (extra + "/"))
            good = False
    if len(sys.argv) == 3:
        print("Title update (%s):" % sys.argv[2])
        good &= check(sys.argv[2], UPDATE)
        if os.path.exists(os.path.join(sys.argv[2], "default.xexp")):
            print("  %-34s REMOVE IT: the game will not start with this file in update/" % "default.xexp")
            good = False
    print("\nAll good." if good else "\nSome files do not match: see above.")
    return 0 if good else 1


if __name__ == "__main__":
    sys.exit(main())
