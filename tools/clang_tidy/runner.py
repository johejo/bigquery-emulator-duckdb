"""Run every check, then tell Bazel which candidate headers were not read."""

import os
from pathlib import Path
import subprocess
import sys


def dependencies(text):
    """Read Clang's make depfile (including escaped whitespace, # and $)."""
    # The target is immaterial; paths after it may themselves contain colons.
    _, separator, prerequisites = text.partition(": ")
    if not separator:
        raise ValueError("clang-tidy did not write a dependency rule")
    prerequisites = prerequisites.replace("\\\n", "")
    words = []
    word = []
    index = 0
    while index < len(prerequisites):
        char = prerequisites[index]
        if char == "\\" and index + 1 < len(prerequisites):
            following = prerequisites[index + 1]
            if following in " \t#\\":
                word.append(following)
                index += 2
                continue
        if char == "$" and prerequisites[index:index + 2] == "$$":
            word.append("$")
            index += 2
            continue
        if char.isspace():
            if word:
                words.append("".join(word))
                word = []
        else:
            word.append(char)
        index += 1
    if word:
        words.append("".join(word))
    if not words:
        raise ValueError("clang-tidy wrote no dependencies")
    return {os.path.normpath(path) for path in words}


def main():
    tool, output, candidates, source, *flags = sys.argv[1:]
    depfile = output + ".d"
    # ClangTool strips ordinary -MD/-MF flags. Pass dependency generation through the
    # preprocessor instead, in the same invocation that performs the checks.
    result = subprocess.run(
        [tool, "--quiet", source, "--", *flags, "-Wp,-MD," + depfile],
        check=False,
    )
    if result.returncode:
        return result.returncode
    used = {os.path.abspath(path) for path in dependencies(Path(depfile).read_text())}
    if os.path.abspath(source) not in used:
        raise ValueError("clang-tidy dependency rule does not contain the checked file")
    # Only compilation headers may be pruned. Configurations and executables are
    # not candidates, and the source must occur in the dependency rule.
    unused = [
        path for path in Path(candidates).read_text().splitlines()
        if os.path.abspath(path) not in used
    ]
    Path(output).write_text("".join(path + "\n" for path in unused))
    return 0


if __name__ == "__main__":
    sys.exit(main())
