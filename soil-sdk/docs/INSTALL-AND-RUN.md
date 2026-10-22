# Running and installing packages in Soil

## Running a package

```sh
soil run Hello.sol
```

The stock Soil kernel is built without `binfmt_misc`, so a `.sol` file cannot be
started by the kernel the way a native ELF binary is. Launch packages with
`soil run`, from the Fluxbox menu, from the pcmanfm desktop icon, or by opening
the file in the file manager: the image registers `.sol` as
`application/x-soil-package` with `soil run %f` as the handler.

`SOIL_STRICT=1 soil run Hello.sol` refuses unsigned/untrusted packages.

## Install

```sh
soil install Hello.sol
soil list
soil launch org.soil.hello
soil remove org.soil.hello
```

`install`:

* copies the package to `~/.local/share/soil/pkg/<id>-<version>.sol`
* extracts the icon to `~/.local/share/soil/icons/<id>.png`
* writes `~/.local/share/applications/soil-<id>.desktop`
* creates `~/Desktop/<Name>.desktop`
* for installer-kind packages, extracts the payload to
  `~/.local/share/soil/opt/<id>/` and runs `install.command`

Everything under `/home` lives on a RAM-backed tmpfs in the default image, so
installs last for the session. Packages on attached storage can be run without
installing.

## Trust

```sh
soil trust publisher.pem     # ~/.config/soil/trust/
soil verify App.sol          # digest + signature report
soil info App.sol            # header + manifest
```

## App data

`~/.local/share/soil/data/<id>/` holds `store.json` and anything the app writes
via the bridge. Removing an app deletes its package, desktop entries, extracted
payload and data directory.

## Command reference

```
soil run [--strict] FILE.sol [args...]
soil install FILE.sol
soil launch ID [args...]
soil list
soil remove ID
soil info FILE.sol|ID
soil verify FILE.sol
soil trust FILE.pem
soil untrust NAME
soil docs
soil help

internal: soil _mount FILE DIR      (root)
          soil _umount DIR          (root)
```

## Desktop integration

* Fluxbox menu: **Hello Soil (sample)** runs the baked sample via `soil run`.
* pcmanfm desktop: **Hello Soil** icon, plus launchers created by `soil install`.
* Opening any `.sol` file in the file manager triggers the registered
  `application/x-soil-package` handler (`soil run %f`).
* `soil docs` opens `/usr/local/share/soil/docs` in pcmanfm.
* Samples shipped with the image live in `/usr/local/share/soil/samples/`.
