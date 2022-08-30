from distutils.core import setup
from distutils.extension import Extension
from Cython.Build import cythonize

import pathlib
source_dirs = ["vidpak", "FiniteStateEntropy/lib/"]
source_files = []
for d in source_dirs:
    for f in pathlib.Path(d).iterdir():
        if f.suffix in {".c", ".pyx"}:
            source_files.append(str(f))

extensions = [
    Extension("vidpak._pack", source_files,
        include_dirs=source_dirs,
        extra_compile_args=["-O3"],
        extra_link_args=["-O3"]),
]

setup(
    name="vidpak",
    version="0.3.2",
    install_requires=["Cython", "numpy"],
    packages=["vidpak"],
    ext_modules=cythonize(extensions,
        compiler_directives={'language_level': "3"}),

    entry_points = {
        "console_scripts": [
            "vidpak=vidpak.tools:main_pack",
            "vidunpak=vidpak.tools:main_unpack",
        ],
    },
)
