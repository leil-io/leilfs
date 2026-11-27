#!/usr/bin/env python3
"""Keep deleted files open until the session-release test kills this process."""

import argparse
import os
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("first", type=int)
    parser.add_argument("count", type=int)
    parser.add_argument("--create-directory", action="store_true")
    args = parser.parse_args()

    if args.create_directory:
        os.mkdir(args.directory)

    paths = [f"{args.directory}/{index}" for index in range(args.first, args.first + args.count)]
    descriptors = [os.open(path, os.O_CREAT | os.O_RDWR) for path in paths]
    for path in paths:
        os.unlink(path)

    print("ready", flush=True)
    time.sleep(1000000)


if __name__ == "__main__":
    main()
