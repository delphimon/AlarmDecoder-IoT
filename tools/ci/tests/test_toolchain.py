"""Regression checks for the reproducible ESP-IDF 5.5 toolchain migration."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
SIMPLEINI_COMMIT = "877f7357d1fa4232f1f3352e5028f99899210b27"


class ToolchainTests(unittest.TestCase):
    def test_framework_and_cmake_are_pinned(self) -> None:
        platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("platform = espressif32@=6.13.0", platformio)
        self.assertIn("cmake_minimum_required(VERSION 3.16)", cmake)

    def test_simpleini_has_one_exact_dependency_source(self) -> None:
        platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        main_cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertNotIn("simpleini", platformio.lower())
        self.assertIn(SIMPLEINI_COMMIT, main_cmake)
        self.assertIn("SimpleIni::SimpleIni", main_cmake)

    def test_fmt_submodule_is_12_2(self) -> None:
        fmt_base = (
            ROOT / "components" / "twilio" / "lib" / "fmt" / "include" / "fmt" / "base.h"
        ).read_text(encoding="utf-8")

        self.assertIn("#define FMT_VERSION 120200", fmt_base)

    def test_primary_components_do_not_create_nested_projects(self) -> None:
        component_root = ROOT / "components"
        for cmake_path in component_root.glob("*/CMakeLists.txt"):
            if cmake_path.parent.name == "stsdk":
                continue
            with self.subTest(component=cmake_path.parent.name):
                self.assertNotIn("project(", cmake_path.read_text(encoding="utf-8"))

    def test_deferred_smartthings_is_not_in_primary_configuration(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        main_cmake = (ROOT / "main" / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertNotIn("CONFIG_STDK_", defaults)
        self.assertNotIn("STDK_CORE_PATH", main_cmake)
        self.assertNotIn("idf::stsdk", main_cmake)

    def test_ci_has_no_obsolete_setuptools_compatibility_shim(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")

        self.assertNotIn("SETUPTOOLS_VERSION", workflow)
        self.assertNotIn("pkg_resources", workflow)
        self.assertIn("'main/CMakeLists.txt'", workflow)


if __name__ == "__main__":
    unittest.main()
