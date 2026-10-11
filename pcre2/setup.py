import os
import sys
from pathlib import Path

from setuptools import Extension, setup

compile_args = []
link_args = []
if sys.platform == "win32":
    compile_args.extend(["/std:c11", "/experimental:c11atomics"])
else:
    compile_args.extend(["-g0", "-fvisibility=hidden"])
if os.environ.get("MSGSPEC_SANITIZE"):
    compile_args.extend(["-fsanitize=address", "-fsanitize=undefined"])
    link_args.extend(["-lasan", "-lubsan"])
if os.environ.get("MSGSPEC_COVERAGE"):
    compile_args.append("--coverage")
    link_args.append("-lgcov")
libraries = ["pthread"] if sys.platform not in {"win32", "emscripten", "wasi"} else []
source = Path("vendor/pcre2/src")

setup(
    ext_modules=[
        Extension(
            "_msgspec_pcre2",
            ["_msgspec_pcre2.c", *map(str, sorted(source.glob("*.c")))],
            include_dirs=[str(source)],
            define_macros=[
                ("PCRE2_CODE_UNIT_WIDTH", "8"),
                ("PCRE2_STATIC", "1"),
                ("SUPPORT_PCRE2_8", "1"),
                ("SUPPORT_UNICODE", "1"),
                ("HAVE_CONFIG_H", "1"),
            ],
            libraries=libraries,
            extra_compile_args=compile_args,
            extra_link_args=link_args,
        )
    ]
)
