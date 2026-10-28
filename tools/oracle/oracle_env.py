#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Vector.
#
# Ghoti.io Vector is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""How a pixel oracle is spelled, so that no tool here spells one itself.

The images are built here from a Containerfile (containers/<name>/): they have
no upstream digest, so the version the oracle prints is checked against
containers/IMAGES every time. An oracle's output is its PNG and its exit status;
nothing of the project is read, copied or linked.

Every refusal to run is OracleUnavailable, which a caller turns into exit
status 2 (oracle-exec) or a counted SKIPPED. It is never an oracle that ran
and said no: a caller that cannot tell those apart records "no oracle" for
"the oracle said no".

The container has no network (--network none): an oracle that could fetch
something mid-run would not be the pin.
"""

import os
import shutil
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
# GHOTI_ORACLE_IMAGES names another table: the self-test plants a version mismatch with it.
IMAGES = os.environ.get("GHOTI_ORACLE_IMAGES") or os.path.join(HERE, "containers", "IMAGES")
ENGINE = os.environ.get("GHOTI_CONTAINER_ENGINE", "podman")

# What each image's probe script prints. The IMAGES version must be inside it.
PROBE = {"resvg": ["resvg-version"], "inkscape": ["inkscape-version"], "browser": ["browser-version"]}


class OracleUnavailable(Exception):
    """The oracle cannot be reached; it did not run."""


_pins = None
_versions = {}


def pins():
    """The IMAGES table: name -> (image, version, description)."""
    global _pins
    if _pins is not None:
        return _pins
    table = {}
    with open(IMAGES, "r", encoding="utf-8") as handle:
        for line in handle:
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 3:
                raise OracleUnavailable(
                    "containers/IMAGES: not three tab-separated fields: %r" % line)
            table[parts[0]] = (parts[1], parts[2], parts[3] if len(parts) > 3 else "")
    _pins = table
    return table


def _have_image(image):
    return subprocess.run([ENGINE, "image", "exists", image],
                          capture_output=True).returncode == 0


def ensure(name):
    """Make the oracle runnable, or raise saying what is missing."""
    if shutil.which(ENGINE) is None:
        raise OracleUnavailable("%s is not on PATH; name another engine with "
                                "GHOTI_CONTAINER_ENGINE" % ENGINE)
    table = pins()
    if name not in table:
        raise OracleUnavailable("no pin for %r in tools/oracle/containers/IMAGES" % name)
    image = table[name][0]
    if not _have_image(image):
        raise OracleUnavailable("image for %s is not present; it is built, not pulled: "
                                "make oracle-images (%s)" % (name, image))


def command(name, argv, ro=(), scratch=()):
    """The argv that runs `argv` inside `name`'s image.

    `ro` directories are mounted read-only and `scratch` read-write, each at its
    own path. The engine is asked whether it is rootless: a rootful one would
    write files the caller cannot remove.
    """
    ensure(name)
    out = [ENGINE, "run", "--rm", "-i", "--network", "none"]
    if not _rootless() and hasattr(os, "getuid"):
        out += ["--user", "%d:%d" % (os.getuid(), os.getgid()), "--env", "HOME=/tmp"]
    for path in ro:
        out += ["--volume", "%s:%s:ro" % (path, path)]
    for path in scratch:
        out += ["--volume", "%s:%s:rw" % (path, path)]
    return out + [pins()[name][0]] + list(argv)


_rootless_cache = None


def _rootless():
    global _rootless_cache
    if _rootless_cache is None:
        try:
            ask = lambda *a: subprocess.run([ENGINE] + list(a), capture_output=True,
                                            text=True, stdin=subprocess.DEVNULL).stdout
        except OSError:
            _rootless_cache = False
            return False
        if "podman" in ask("--version").lower():
            _rootless_cache = ask("info", "--format", "{{.Host.Security.Rootless}}").strip() == "true"
        else:
            _rootless_cache = "rootless" in ask("info", "--format", "{{json .SecurityOptions}}")
    return _rootless_cache


def version(name):
    """What the oracle says it is. Runs it; the answer is cached."""
    if name in _versions:
        return _versions[name]
    done = subprocess.run(command(name, PROBE[name]), capture_output=True, text=True,
                          stdin=subprocess.DEVNULL)
    lines = done.stdout.strip().splitlines()
    text = lines[0] if lines else ""
    if done.returncode != 0:
        raise OracleUnavailable("%s probe exited %s: %r" % (name, done.returncode, text))
    _versions[name] = text
    return text


def check_pin(name):
    """Raise unless the oracle's version matches containers/IMAGES."""
    want = pins()[name][1] if name in pins() else None
    if want is None:
        raise OracleUnavailable("no pin for %r in tools/oracle/containers/IMAGES" % name)
    got = version(name)
    if want not in got:
        raise OracleUnavailable("%s: IMAGES says %s and it answers %r" % (name, want, got))
    return got
