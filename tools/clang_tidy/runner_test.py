"""Dependency paths must match Bazel inputs without losing make escaping."""

import unittest

from tools.clang_tidy.runner import dependencies


class DependenciesTest(unittest.TestCase):
    def test_clang_paths(self):
        self.assertEqual(
            dependencies(
                "source.o: src/source.cc \\\n"
                " ./src/header.h external/lib/header.h /sdk/include/header.h\n"
            ),
            {"src/source.cc", "src/header.h", "external/lib/header.h", "/sdk/include/header.h"},
        )

    def test_escaped_paths(self):
        self.assertEqual(
            dependencies("source.o: dir/with\\ space.h hash\\#.h dollar$$.h quote'.h colon:name.h\n"),
            {"dir/with space.h", "hash#.h", "dollar$.h", "quote'.h", "colon:name.h"},
        )

    def test_invalid_depfile(self):
        for text in ("", "source.o:", "source.o: \n"):
            with self.subTest(text=text), self.assertRaises(ValueError):
                dependencies(text)


if __name__ == "__main__":
    unittest.main()
