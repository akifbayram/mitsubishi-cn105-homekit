#!/usr/bin/env python3
"""Derive the version string baked into the firmware image as PROJECT_VER.

This lives in a script rather than inline in CMakeLists.txt because it is
load-bearing and was untestable from there. It decides whether a release binary
self-reports "-dirty", and main/CMakeLists.txt feeds the result to both
FW_VERSION (the HomeKit firmware revision and the Improv Serial identity) and
the embedded web UI, where web/index.html's cmpVer() compares it against the
published manifest to decide whether to offer an update. test/project_ver/
drives this same file over throwaway repos.

The inline version this replaces was wrong in several ways at once; each rule
below names the failure it prevents.
"""

import argparse
import os
import re
import subprocess
import sys

# A benign dependencies.lock line: idf-component-manager's LockManager.dump()
# assigns lock.target = get_env_idf_target() on every configure and rewrites the
# file when the serialised text differs. CI builds esp32c6 and esp32s3 from one
# tagged checkout and the committed file records esp32c6, so the esp32s3 job
# always left the lock modified -- which is why every v0.2.5-beta.* and the
# v0.2.5 m5atoms3-lite binary shipped "-dirty".
TARGET_LINE = r"^target: [a-z0-9]+$"

# Only release tags. Without this any reachable tag becomes the firmware version,
# and one whose name starts with a digit parses as a core version above every
# real release, which pins the device on "up to date" forever.
TAG_GLOB = "v[0-9]*"


class GitError(RuntimeError):
    pass


def git(repo, *args):
    """Run git and return stdout. Raises GitError on any nonzero exit.

    OSError is folded in too: a missing git binary raises FileNotFoundError,
    which would otherwise escape as a traceback and fail the build outright
    rather than falling back the way every other git failure here does.
    """
    try:
        p = subprocess.run(("git",) + args, cwd=repo, capture_output=True, text=True)
    except OSError as exc:
        raise GitError("git %s -> %s" % (" ".join(args), exc))
    if p.returncode != 0:
        raise GitError(
            "git %s -> %d: %s" % (" ".join(args), p.returncode, p.stderr.strip())
        )
    return p.stdout


def describe(repo):
    """The tag this build sits on, or None when no release tag is reachable.

    No --always. Its bare-SHA fallback bakes a string parseVer() reads as a
    version number -- "39942af" parses as core [39942], above every release that
    will ever ship -- and .github/workflows/build.yml checks out at the default
    fetch-depth, which fetches no tags, so that fallback is its normal case.
    --abbrev=7 pins the g<sha> tail against a lowered core.abbrev, which would
    drop below parseVer()'s [0-9a-f]{7,} and lose the dev flag on every
    between-tags build.
    """
    try:
        out = git(repo, "describe", "--tags", "--match", TAG_GLOB, "--abbrev=7").strip()
    except GitError:
        return None
    return out or None


def warn_on_tag_tie(repo):
    """Warn when HEAD carries more than one release tag.

    Only reached when no --release-tag was supplied, so a release build never
    depends on this. It warns rather than refuses because a promotion commit
    carrying both the beta and the stable tag is a normal thing to build.
    """
    try:
        tags = [t for t in git(repo, "tag", "--points-at", "HEAD",
                               "--list", TAG_GLOB).split() if t]
    except GitError:
        return
    if len(tags) > 1:
        sys.stderr.write(
            "project_ver: HEAD carries %d release tags (%s); git describe picks "
            "one by tagger date\n" % (len(tags), ", ".join(sorted(tags)))
        )


def tree_dirty(repo):
    """True when tracked files differ from HEAD, ignoring the lock.

    git status, not git diff-index: diff-index trusts the index's stat cache and
    never refreshes it, so a checkout that only skewed an mtime reported a
    "-dirty" that git describe --dirty never did. status refreshes and writes the
    index back, which is what --dirty used to do for us. It also reports
    dirtiness on stdout and keeps every nonzero exit for real failures, so a
    locked index or a safe.directory refusal can no longer look like a modified
    tree.

    Untracked files are ignored, matching git describe --dirty and _tree_dirty()
    in the dial's tools/publish_fw.py; a build leaves sdkconfig, build/ and
    include/ behind (see .gitignore). --ignore-submodules=untracked keeps a moved
    esp-homekit-sdk SHA and any edit to its tracked sources dirty -- both compile
    into the image -- while ignoring build litter inside it, and pins that
    against a diff.ignoreSubmodules setting in the environment.

    Both pathspecs are relative to the project directory, not to the repository
    root, so vendoring this tree under another repo keeps the scope and the
    exclusion pointing at this project's own files.

    Known divergence in that vendored case: the tag comes from git describe,
    which searches the whole enclosing repository, while this check is scoped to
    the project subdirectory — so an edit outside the subdirectory would not
    mark the build dirty even though the tag it is named after moved. Nothing
    vendors this tree yet; revisit the scoping when something does, rather than
    guessing now which half should win.
    """
    return bool(git(repo, "status", "--porcelain", "--untracked-files=no",
                    "--ignore-submodules=untracked",
                    "--", ".", ":(exclude)dependencies.lock").strip())


def lock_dirty(repo):
    """True when dependencies.lock differs from HEAD by more than the target stamp.

    Excluding the lock by path -- what this replaces -- also hid every real change
    to it: resolved component versions, component_hash, manifest_hash and the
    recorded idf version. main/idf_component.yml bounds mdns only as "~1.11.3",
    so a registry patch release rewrites the resolved set, and the committed lock
    exists precisely so the tagged binary is built from the set that was tested.

    -I ignores a change only when every line in its hunk matches, and the
    pathspec confines that tolerance to this one file. Deleting a
    direct_dependencies item yields the patch line "-- espressif/mdns", which a
    naive "skip lines starting with -" filter would drop as a diff header; git
    does the comparison here, so that trap does not arise.

    -I is hunk-shaped, not positional, so it would also forgive an inserted
    second target: line. exactly_one_target_line() closes that.
    """
    return bool(git(repo, "diff", "--name-only", "-I" + TARGET_LINE,
                    "HEAD", "--", "dependencies.lock").strip())


def target_stamp_malformed(repo):
    """True when the working-tree lock does not carry exactly one target: line.

    The tolerance in lock_dirty() is that a single top-level target: line may
    differ in value. Anything else -- no stamp, or a second one inserted -- is
    outside what idf-component-manager produces and must not be forgiven.
    """
    path = os.path.join(repo, "dependencies.lock")
    if not os.path.exists(path):
        return False  # absent entirely; the diff above already judged it
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    return len(re.findall(TARGET_LINE, text, re.MULTILINE)) != 1


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repo", default=".", help="project directory to inspect")
    ap.add_argument(
        "--release-tag", default="",
        help="the tag being released, when CI knows it. Used verbatim as the "
             "version instead of git describe; the dirty checks still run.",
    )
    args = ap.parse_args()
    repo = args.repo

    if args.release_tag:
        # firmware-release.yml hands over the pushed tag, because only it knows
        # which tag was pushed: git describe breaks a tie between two tags on
        # one commit by tagger date, and v0.2.5 and v0.2.5-beta.3 both point at
        # c4f9088. The published manifest comes from the same github.ref_name,
        # so the baked version and the manifest cannot disagree.
        #
        # The dirty checks below still run on this path. Taking the tag as
        # proof of cleanliness would be exactly the hole this script exists to
        # close: a release build is where an unexpected dependencies.lock
        # change matters most, since that is the binary people install.
        version = args.release_tag
    else:
        version = describe(repo)
        if version is None:
            # No release tag reachable: a shallow CI checkout, or a fresh clone
            # without tags. 0.0.0-dev sorts below every release, so such a build
            # offers an update rather than claiming to be current.
            print("0.0.0-dev")
            return 0
        warn_on_tag_tie(repo)

    try:
        dirty = tree_dirty(repo) or lock_dirty(repo) or target_stamp_malformed(repo)
    except (GitError, OSError) as exc:
        # Cleanliness could not be established. Say "-dirty" rather than emit a
        # bare tag: parseVer() sets dev=true on a -dirty string, so the updater
        # treats the build as a dev build instead of as that release.
        sys.stderr.write("project_ver: %s -- assuming a dirty tree\n" % exc)
        dirty = True

    print((version + "-dirty") if dirty else version)
    return 0


if __name__ == "__main__":
    sys.exit(main())
