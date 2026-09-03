# BGP++

[![Build BGP](https://github.com/facebook/BGP/actions/workflows/build.yml/badge.svg)](https://github.com/facebook/BGP/actions/workflows/build.yml)
[![License: Apache 2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)

BGP++ is Meta's C++ implementation of the Border Gateway Protocol. It supports
Meta's Data Center and Express Backbone networks and can be used as a
standalone routing daemon or as a collection of BGP libraries.

This repository provides two mutually exclusive build variants:

- **DC**: Data Center integration using the FBOSS Agent and FSDB.
- **BB**: Express Backbone integration using FibEbb, Open/R, and Linux netlink.

> **Note:** The daemons require platform FIB services and a valid BGP
> configuration at runtime. Building this repository does not produce a
> complete switch image or a turnkey router deployment.

## Build variants

| Variant | CMake value | FBOSS profile | CMake target | Installed binary | Platform integration |
|---|---|---|---|---|---|
| DC | `dc` | `fsdb_client` | `bgp_bin` | `sbin/bgp` | FBOSS Agent and FSDB |
| BB | `bb` | `exported_libraries` | `bgp_bb_bin` | `sbin/bgp_bb` | FibEbb, Open/R, and netlink |

`dc` is the default CMake variant. GitHub CI always sets the variant explicitly.

Only one variant is configured in each CMake tree. Use separate scratch
directories for DC and BB builds.

## Requirements

The supported OSS build uses
[`getdeps.py`](build/fbcode_builder/README.md), which downloads and builds BGP++
and its dependencies.

The build requires:

- Linux. GitHub CI runs on Ubuntu.
- Python 3.
- Git.
- A C++20-capable compiler. The dependency manifest includes GCC 12.
- CMake 3.19 or newer when configuring the dependency profiles directly.
- Permission to install system packages when using `--allow-system-packages`.
- Sufficient memory and disk space to build FBOSS, Open/R, Folly, FBThrift,
  and their dependencies.

GitHub CI uses 18 parallel jobs on a 32-core runner. Use a lower value on
machines with less memory.

## Clone the repository

```bash
git clone https://github.com/facebook/BGP.git
cd BGP

# Adjust for the available CPU and memory.
JOBS="${JOBS:-8}"
```

## Scratch directories

Use a separate scratch directory outside the Git checkout for each variant:

| Variant | Example |
|---|---|
| DC | `/tmp/${USER}-bgp-build-dc` |
| BB | `/tmp/${USER}-bgp-build-bb` |

Do not use paths inside the repository, such as `./build-output` or
`./.getdeps`, and do not share one scratch directory between DC and BB.

There are two reasons:

1. `getdeps.py` applies dependency patches relative to the enclosing Git
   repository. An in-repository scratch path can make patches resolve against
   the BGP checkout and fail with errors such as `No such file or directory`.
2. DC and BB use different `BGP_BUILD_VARIANT` and `FBOSS_BUILD_PROFILE`
   values. Reusing one scratch directory can retain dependency configuration
   from the other variant and cause incorrect configure or link results.

When switching variants, use the matching existing scratch directory or create
a new one.

## Build the DC variant

The DC build uses the FBOSS `fsdb_client` profile and produces `bgp`.

```bash
DC_SCRATCH="${DC_SCRATCH:-/tmp/${USER}-bgp-build-dc}"

sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=fsdb_client \
  python3 build/fbcode_builder/getdeps.py \
  --allow-system-packages \
  --scratch-path "$DC_SCRATCH" \
  build \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"dc"}' \
  --src-dir=. \
  --num-jobs "${JOBS:-8}" \
  bgp
```

`BUILD_SAI_FAKE=1` builds the FBOSS dependency without a vendor SAI SDK.
`--src-dir=.` ensures that the current BGP checkout is built.

## Build the BB variant

The BB build uses the smaller FBOSS `exported_libraries` profile and produces
`bgp_bb`.

```bash
BB_SCRATCH="${BB_SCRATCH:-/tmp/${USER}-bgp-build-bb}"

sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=exported_libraries \
  python3 build/fbcode_builder/getdeps.py \
  --allow-system-packages \
  --scratch-path "$BB_SCRATCH" \
  build \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"bb"}' \
  --src-dir=. \
  --num-jobs "${JOBS:-8}" \
  bgp
```

The BB profile builds only these FBOSS libraries and their dependencies:

- `nodebase`
- `radix_tree`
- `exponential_back_off`
- `log_thrift_call`
- `alert_logger`

The BGP manifest selects Open/R's `exported_libraries` profile automatically.

## Run tests

Tests must use the same variant, FBOSS profile, scratch directory, and build
type as the corresponding build.

### DC tests

```bash
DC_SCRATCH="${DC_SCRATCH:-/tmp/${USER}-bgp-build-dc}"

sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=fsdb_client \
  python3 build/fbcode_builder/getdeps.py \
  --allow-system-packages \
  --scratch-path "$DC_SCRATCH" \
  test \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"dc"}' \
  --src-dir=. \
  --num-jobs "${JOBS:-8}" \
  bgp
```

### BB tests

```bash
BB_SCRATCH="${BB_SCRATCH:-/tmp/${USER}-bgp-build-bb}"

sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=exported_libraries \
  python3 build/fbcode_builder/getdeps.py \
  --allow-system-packages \
  --scratch-path "$BB_SCRATCH" \
  test \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"bb"}' \
  --src-dir=. \
  --num-jobs "${JOBS:-8}" \
  bgp
```

## Locate the installed binaries

Use `show-inst-dir` with the same scratch directory, CMake variant, and
privilege as the corresponding build.

### DC

```bash
DC_SCRATCH="${DC_SCRATCH:-/tmp/${USER}-bgp-build-dc}"

DC_INSTALL_DIR="$(
  sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=fsdb_client \
  python3 build/fbcode_builder/getdeps.py \
  --scratch-path "$DC_SCRATCH" \
  show-inst-dir \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"dc"}' \
  --src-dir=. \
  bgp
)"

sudo "$DC_INSTALL_DIR/sbin/bgp" --version
```

### BB

```bash
BB_SCRATCH="${BB_SCRATCH:-/tmp/${USER}-bgp-build-bb}"

BB_INSTALL_DIR="$(
  sudo env \
  BUILD_SAI_FAKE=1 \
  FBOSS_BUILD_PROFILE=exported_libraries \
  python3 build/fbcode_builder/getdeps.py \
  --scratch-path "$BB_SCRATCH" \
  show-inst-dir \
  --extra-cmake-defines='{"BGP_BUILD_VARIANT":"bb"}' \
  --src-dir=. \
  bgp
)"

sudo "$BB_INSTALL_DIR/sbin/bgp_bb" --version
```

`getdeps.py` installs the selected daemon under `sbin/`, generated and public
headers under `include/`, and reusable static libraries under `lib/`.

## Build only the daemon

The commands above build the complete selected variant, including its test
targets. For a daemon-only build, add these options after the `build`
subcommand:

| Variant | Additional options |
|---|---|
| DC | `--no-tests --cmake-target bgp_bin` |
| BB | `--no-tests --cmake-target bgp_bb_bin` |

Rerun the complete build without `--no-tests` before invoking `getdeps.py test`.

## Build types

`getdeps.py` supports:

- `Debug`
- `RelWithDebInfo` — default
- `MinSizeRel`
- `Release`

Pass the same value to every build, test, and path-query command:

```bash
--build-type Debug
```

## Runtime integration

Both daemons accept:

- `--config=<path>` for the BGP configuration.
- `--policy=<path>` for an optional separate policy configuration.
- `--help` to list all registered command-line flags.
- `--version` for build information.

The main configuration schema is
[`configerator/structs/neteng/fboss/bgp/bgp_config.thrift`](configerator/structs/neteng/fboss/bgp/bgp_config.thrift).
Policy schemas are under
[`configerator/structs/neteng/bgp_policy/thrift/`](configerator/structs/neteng/bgp_policy/thrift/).

A test-only standalone configuration is available at
[`neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json`](neteng/fboss/bgp/cpp/tests/sample_configs/stand_alone_conf.json).
It is a test fixture, not a production deployment template.

During normal startup:

- `bgp` waits for the FBOSS Agent FIB service.
- `bgp_bb` waits for both FibEbb and the Open/R FIB agent.
- `--platform=dev` skips the platform-readiness wait for development, but does
  not provide the missing FIB services.

## Repository layout

The paths below are relative to the GitHub repository root.

| Path | Purpose |
|---|---|
| [`CMakeLists.txt`](CMakeLists.txt) | Top-level CMake entry point. It selects `BGP_BUILD_VARIANT=dc\|bb`. |
| [`cmake/`](cmake/) | Defines the shared, DC, and BB CMake targets. |
| [`neteng/fboss/bgp/`](neteng/fboss/bgp/) | Contains the BGP source, Thrift services, tests, and sample configurations. |
| [`configerator/`](configerator/) | Contains the public BGP configuration and routing-policy Thrift schemas. |
| [`common/`](common/) | Contains the shared network and FB303 headers and schemas used by BGP. |
| [`build/fbcode_builder/`](build/fbcode_builder/) | Provides `getdeps.py`, dependency manifests, and shared CMake helpers. |
| [`.github/workflows/`](.github/workflows/) | Contains the GitHub Actions workflow. |
| [`CONTRIBUTING.md`](CONTRIBUTING.md) | Describes how to contribute to the repository. |
| [`CODE_OF_CONDUCT.md`](CODE_OF_CONDUCT.md) | Defines the project code of conduct. |
| [`LICENSE`](LICENSE) | Contains the Apache License 2.0 text. |
