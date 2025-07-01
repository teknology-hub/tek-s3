# Building tek-s3

This guide assumes that you have installed `curl` program, and `tar` or `git`, which in most systems are either pre-installed or can be easily acquired. On Windows, using [MSYS2](https://www.msys2.org/) is required, preferably with CLANG64 environment as the most tested during development.

## 1. Install dependencies

Package names and commands used to install them vary across systems, so they will net be given here for the time being.

### Toolchain requirements

- C and C++ compilers with decent support for C23/C++23 with GNU extensions (i.e. `-std=gnu23` and `-std=gnu++23`), most notable are GCC 13+ and Clang 18+.
- [Meson build system](https://mesonbuild.com) - must be installed to build the project.
- [CMake](https://cmake.org) - may be required to be present to correctly find certain dependencies.

### Dependencies

Here's the list of libraries that tek-s3 depends on:

|Library|Usage|
|-|-|
|[libbrotlienc](https://github.com/google/brotli) (optional)|Brotli compression of the manifest|
|[libsystemd](https://github.com/systemd/systemd) (optional)|Notifying systemd of current service status|
|[libwebsockets](https://libwebsockets.org)|WebSocket connections for Steam CM client|
|[libzstd](https://github.com/facebook/zstd) (optional)|Zstandard compression of the manifest|
|[RapidJSON](https://rapidjson.org/)|JSON serialization and parsing|
|[tek-steamclient](https://github.com/teknology-hub/tek-steamclient)|Communication with Steam CM servers|
|[ValveFileVDF](https://github.com/TinyTinni/ValveFileVDF)|Valve Data File parsing. Provided via Meson wrap file in the repository, doesn't need to be installed separately|
|[zlib](https://www.zlib.net) or [zlib-ng](https://github.com/zlib-ng/zlib-ng)|DEFLATE compression of the manifest|

## 2. Get source code

Clone this repository:
```sh
git clone https://github.com/teknology-hub/tek-s3.git
cd tek-s3
```
, or download a point release e.g.
```sh
curl -LOJ https://github.com/teknology-hub/tek-s3/releases/download/v1.0.0/tek-s3-1.0.0.tar.gz`
tar -xzf tek-s3-1.0.0.tar.gz
cd tek-s3-1.0.0
```

## 3. Setup build directory

At this stage you can set various build options. tek-s3's own options are listed in [meson.options](https://github.com/teknology-hub/tek-s3/blob/main/meson.options), other available options are described in [Meson documentation](https://mesonbuild.com/Commands.html#setup).
The simplest case that uses default options, debugoptimized build type (which uses -O2 optimization level instead of often overrated -O3 in release), and strips tek-s3 binary of debug information during installing:
```sh
meson setup build --buildtype debugoptimized -Dstrip=true
```
On Windows in MSYS2 you way also want to set prefix to the one matching your environment, e.g for CLANG64 the option would be `--prefix=/clang64`.

## 4. Compile and install the project

```sh
meson install -C build
```
This will compile source files and install the tek-s3 binary into a system location, after which you can use it. If you're on MSYS2, keep in in mind that this binary cannot be used outside of MSYS2 environment unless you copy **all** DLLs that it depends on into its directory. To circumvent that, you'd have to link all dependencies statically, which is not possible with official MSYS2 packages at the moment of writing this due to some of them not providing static library files, or correct package metadata for static linking, so those would have to be rebuilt with custom options. Doing so is possible (release binaries are built this way), but it's way out of the scope of this guide.
