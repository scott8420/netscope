# NetScope

A GNOME network browser. It finds what is on your network, shows what each
device offers, explains in plain English why one can't be reached, and connects
with one click. Mounted shares appear in Files (Nautilus) like any other volume.

It exists because Linux has the pieces but nothing ties them together, and
because two Apple Time Capsules that "couldn't connect" turned out to need a
routing workaround and an AFP client that GNOME doesn't have.

**Status: scaffold.** This is the Cairn starter renamed. Discovery, diagnosis
and connecting arrive in later sessions.

## Build and run

```
./build.sh
./build/netscope_selftest
./build/netscope
```

Deps (Fedora): `gtkmm4.0-devel spdlog-devel cmake gcc-c++ glib2-devel`.
Deps (Ubuntu): `libgtkmm-4.0-dev libspdlog-dev cmake g++`.
`ninja`, `ccache` and `mold` are used when present.

## Install

```
./install.sh            # ~/.local, no sudo
./install.sh --system   # /usr/local
./install.sh --build    # build first
./uninstall.sh          # same --system / --prefix; --purge removes settings too
```

## License

MIT. `third_party/json.hpp` is nlohmann/json, also MIT.
