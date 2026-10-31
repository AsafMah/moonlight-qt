"""Measure the owner's cancellation fixtures linked against the real library.

Only loopback peers and the library's deterministic socket/resolver hooks are
used. The supplied common-c files are read, never edited.
"""

import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
COMMON = ROOT / "moonlight-common-c/moonlight-common-c"


def main():
    fixture = (COMMON / "tests/ConnectionCancellationTest.c").read_text(encoding="utf-8")
    for header in ("Limelight-internal.h", "PlatformSockets.h"):
        fixture = fixture.replace(f'"../src/{header}"', f'"{COMMON.as_posix()}/src/{header}"')
    for clock, bound in (("started", 500), ("interruptStarted", 500), ("started", 250)):
        check = f"CHECK(PltGetMillis() - {clock} < {bound});"
        fixture = fixture.replace(
            check,
            f'printf("LATENCY %s %llu ms\\n", __func__, '
            f'(unsigned long long)(PltGetMillis() - {clock}));\n    {check}')
    fixture = fixture.replace(
        "int main(void) {",
        r"""int main(void) {
    SERVER_INFORMATION expiredServer = { 0 };
    STREAM_CONFIGURATION expiredConfig;
    LiInitializeStreamConfiguration(&expiredConfig);
    LiPrepareConnectionAttempt(100);
    PltSleepMs(150);
    CHECK(LiStartConnection(&expiredServer, &expiredConfig, NULL, NULL, NULL,
                           NULL, 0, NULL, 0) == ETIMEDOUT);
    printf("Preparation deadline was not renewed on library entry\\n");
""")
    with tempfile.TemporaryDirectory(prefix="moonlight-real-cancellation-") as directory:
        path = Path(directory)
        (path / "latency.c").write_text(fixture, encoding="utf-8")
        (path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.15)\n"
            "project(client_cancellation_latency LANGUAGES C)\n"
            "set(BUILD_TESTING ON CACHE BOOL \"\" FORCE)\n"
            "set(BUILD_SHARED_LIBS OFF CACHE BOOL \"\" FORCE)\n"
            f'add_subdirectory("{COMMON.as_posix()}" common)\n'
            "add_executable(client-cancellation-latency latency.c)\n"
            "target_include_directories(client-cancellation-latency PRIVATE\n"
            f'  "{COMMON.as_posix()}/enet/include"\n'
            "  $<TARGET_PROPERTY:moonlight-common-c,INCLUDE_DIRECTORIES>)\n"
            "target_compile_definitions(client-cancellation-latency PRIVATE\n"
            "  $<TARGET_PROPERTY:moonlight-common-c,COMPILE_DEFINITIONS>)\n"
            "target_compile_options(client-cancellation-latency PRIVATE\n"
            "  $<TARGET_PROPERTY:moonlight-common-c,COMPILE_OPTIONS>)\n"
            "target_link_libraries(client-cancellation-latency PRIVATE moonlight-common-c)\n",
            encoding="utf-8")
        command = ["cmake", "-S", str(path), "-B", str(path / "build"),
                   "-DCMAKE_BUILD_TYPE=Release"]
        if os.name == "nt":
            command += ["-G", "NMake Makefiles",
                        f"-DOPENSSL_INCLUDE_DIR={ROOT / 'libs/windows/include/x64'}",
                        f"-DOPENSSL_CRYPTO_LIBRARY={ROOT / 'libs/windows/lib/x64/libcrypto.lib'}",
                        f"-DOPENSSL_SSL_LIBRARY={ROOT / 'libs/windows/lib/x64/libssl.lib'}"]
        subprocess.run(command, check=True, timeout=120, capture_output=True, text=True)
        subprocess.run(["cmake", "--build", str(path / "build"), "--target",
                        "client-cancellation-latency"], check=True, timeout=180,
                       capture_output=True, text=True)
        if os.name == "nt":
            runner = Path(r"E:\Code\moonlight-tools\run-test-exe.ps1")
            if not runner.is_file():
                runner = ROOT / "scripts/run-built-test.ps1"
            command = ["pwsh", "-NoProfile", "-File", str(runner), "-Exe",
                       str(path / "build/client-cancellation-latency.exe"), "-Platform", "offscreen"]
        else:
            command = [str(path / "build/client-cancellation-latency")]
        environment = os.environ.copy()
        if os.name == "nt":
            environment["PATH"] = str(ROOT / "libs/windows/lib/x64") + os.pathsep + environment["PATH"]
        result = subprocess.run(command, check=True, timeout=60, env=environment,
                                capture_output=True, text=True)
        print(result.stdout, end="")
        print(result.stderr, end="")
        required = {"testPreparedCancellationAndDeadline", "testConnectWaitInterruptAndDeadlineClamp",
                    "testResolverCancellationOwnershipAndRecovery", "testStunReceiveInterrupt",
                    "testRtspReceiveInterrupt"}
        measurements = {}
        for line in result.stdout.splitlines():
            if line.startswith("LATENCY "):
                _, phase, elapsed, _ = line.split()
                elapsed = int(elapsed)
                if elapsed >= 1000:
                    raise AssertionError(f"{phase} interrupt-to-return exceeded 1 second: {elapsed}")
                measurements.setdefault(phase, []).append(elapsed)
        if required - measurements.keys():
            raise AssertionError(f"Missing actual phase measurements: {required - measurements.keys()}")
        summary = re.search(r"(\d+) checks, 0 failures", result.stdout)
        if summary is None or int(summary[1]) < 58:
            raise AssertionError("The complete owner cancellation fixture did not pass")
        print("Real-library phase latencies verified below 1 second")


if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as error:
        print(error.stdout or "")
        print(error.stderr or "")
        raise
