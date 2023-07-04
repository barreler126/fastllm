import glob
import os.path
from setuptools import setup, Extension
from setuptools import find_packages

import sys
import argparse

parser = argparse.ArgumentParser(description='build pyfastllm wheel')
parser.add_argument('--cuda', dest='cuda', action='store_true', default=False,
                    help='build with cuda support')
args, unknown = parser.parse_known_args()
sys.argv = [sys.argv[0]] + unknown

__VERSION__ = "'0.1.2'"
BASE_DIR = os.path.dirname(os.path.dirname(__file__))

ext_modules = []
try:
    from pybind11.setup_helpers import Pybind11Extension
    # could only be relative paths, otherwise the `build` command would fail if you use a MANIFEST.in to distribute your package
    # only source files (.cpp, .c, .cc) are needed
    # 
    source_files = glob.glob(os.path.join(BASE_DIR, "src/**/*.cpp"), recursive=True)
    # source_files.append(os.path.join(BASE_DIR, "src/devices/cpu/cpudevice.cpp"))

    # remove cuda source 
    for file in source_files:
        if file.endswith('cudadevice.cpp'):
            source_files.remove(file)

    extra_compile_args = ["-w", "-DPY_API"]
    # If any libraries are used, e.g. libabc.so
    include_dirs = [os.path.join(BASE_DIR, "include/"), os.path.join(BASE_DIR, "include/device/cpu/"), os.path.join(BASE_DIR, "include/models"), os.path.join(BASE_DIR, "include/utils")]
    library_dirs = []
    # (optional) if the library is not in the dir like `/usr/lib/`
    # either to add its dir to `runtime_library_dirs` or to the env variable "LD_LIBRARY_PATH"
    # MUST be absolute path
    runtime_library_dirs = []
    libraries = []

    if args.cuda:
        assert "Not Implement Yet!"
        # source_files.append("src/devices/cpu/cpudevice.cpp", )
        runtime_library_dirs.append("/usr/local/cuda/lib64/")
        libraries.append("cublas")

    ext_modules = [
        Pybind11Extension(
            "pyfastllm", # depends on the structure of your package
            source_files,
            define_macros=[('VERSION_INFO', __VERSION__)],
            include_dirs=include_dirs,
            library_dirs=library_dirs,
            runtime_library_dirs=runtime_library_dirs,
            libraries=libraries,
            extra_compile_args=extra_compile_args,
            cxx_std=17,
            language='c++'
        ),
    ]
except Exception as e:
    print(f"some errors happened: ")
    print(e)
    sys.exit(1)


cmdclass = {}
setup(
    name='fastllm',  # used by `pip install`
    version=eval(__VERSION__),
    description='python api for fastllm',
    long_description='',
    ext_modules=ext_modules,
    packages = find_packages(), # the directory would be installed to site-packages
    cmdclass=cmdclass,
    entry_points = {
        'console_scripts': [
            'fastllm-convert = fastllm.convert:convert_main',
        ]
    },
    setup_requires=["pybind11"],
    install_requires=[""],
    python_requires='>=3.6',
    include_package_data=False,
    zip_safe=False,
)