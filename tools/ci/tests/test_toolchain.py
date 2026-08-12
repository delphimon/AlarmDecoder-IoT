"""Regression checks for the reproducible ESP-IDF 6.0 toolchain migration."""

from __future__ import annotations

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[3]
SIMPLEINI_COMMIT = "877f7357d1fa4232f1f3352e5028f99899210b27"


class ToolchainTests(unittest.TestCase):
    def test_framework_and_cmake_are_pinned(self) -> None:
        platformio = (ROOT / "platformio.ini").read_text(encoding="utf-8")
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")

        self.assertIn("platform = espressif32@=7.0.0", platformio)
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

    def test_idf6_external_components_are_exactly_pinned(self) -> None:
        manifest = (ROOT / "main" / "idf_component.yml").read_text(encoding="utf-8")
        lock = (ROOT / "dependencies.lock").read_text(encoding="utf-8")

        self.assertIn('espressif/cjson: "==1.7.19~2"', manifest)
        self.assertIn('espressif/mqtt: "==1.0.0"', manifest)
        self.assertIn("version: 1.7.19~2", lock)
        self.assertIn("version: 1.0.0", lock)
        self.assertIn("version: 6.0.0", lock)

    def test_removed_idf5_apis_are_not_used_by_primary_sources(self) -> None:
        source_paths = list((ROOT / "main").glob("*.cpp"))
        source_paths.extend((ROOT / "components").glob("*/**/*.cpp"))
        source_paths = [path for path in source_paths if "stsdk" not in path.parts]
        source = "\n".join(path.read_text(encoding="utf-8") for path in source_paths)

        for removed in (
            "esp_eth_phy_new_lan87xx",
            "HTTP_EVENT_HEADER_SENT",
            "mbedtls_sha256_",
            '"mbedtls/entropy.h"',
            '"mbedtls/ctr_drbg.h"',
        ):
            with self.subTest(api=removed):
                self.assertNotIn(removed, source)

    def test_primary_board_preserves_rmii_clock_output(self) -> None:
        defaults = (ROOT / "sdkconfig.defaults").read_text(encoding="utf-8")
        kconfig = (ROOT / "main" / "Kconfig.projbuild").read_text(encoding="utf-8")
        device_control = (ROOT / "main" / "device_control.cpp").read_text(encoding="utf-8")

        self.assertIn("CONFIG_AD2IOT_ETH_RMII_CLK_OUTPUT=y", defaults)
        self.assertIn("CONFIG_AD2IOT_ETH_RMII_CLK_GPIO=17", defaults)
        self.assertIn("config AD2IOT_ETH_RMII_CLK_OUTPUT", kconfig)
        self.assertIn("emac_config.clock_config.rmii.clock_mode = EMAC_CLK_OUT", device_control)
        self.assertIn("emac_config.clock_config.rmii.clock_gpio = CONFIG_AD2IOT_ETH_RMII_CLK_GPIO", device_control)
        self.assertNotIn("ESP_ERROR_CHECK(esp_eth_driver_install", device_control)

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

    def test_ci_pins_current_build_runtime_dependencies(self) -> None:
        workflow = (ROOT / ".github" / "workflows" / "build.yml").read_text(encoding="utf-8")

        self.assertNotIn("SETUPTOOLS_VERSION", workflow)
        self.assertNotIn("pkg_resources", workflow)
        self.assertIn("PLATFORMIO_CORE_VERSION: 6.1.19", workflow)
        self.assertIn("INTELHEX_VERSION: 2.3.0", workflow)
        self.assertIn('"intelhex==${INTELHEX_VERSION}"', workflow)
        self.assertIn("pio-v3-", workflow)
        self.assertNotIn("restore-keys:", workflow)
        self.assertIn("'main/CMakeLists.txt'", workflow)
        self.assertIn("'main/idf_component.yml'", workflow)
        self.assertIn("'dependencies.lock'", workflow)


if __name__ == "__main__":
    unittest.main()
