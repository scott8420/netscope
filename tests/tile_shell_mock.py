# tile_shell_mock.py -- stand-ins for netscoped and the NetScope app's
# org.freedesktop.Application, for tests/gui_tile_shell.sh (milestone 11;
# Samson's s011 harness). Logs each call to $1.
# Pins: Data on tc01 (AFP, not mounted -> Mount says "password") and pub on
# raspi (SMB, mounted). Mount on SMB mounts; Unmount / UnmountAll unmount.
# TestKeyring() (the harness only, milestone 12): from then on an AFP Mount
# takes 3 s and mounts, as netscoped does with a password in Keyring.
import sys
import dbus, dbus.service, dbus.mainloop.glib
from gi.repository import GLib
dbus.mainloop.glib.DBusGMainLoop(set_as_default=True)
out = sys.argv[1]
IF = "io.github.scott8420.NetScope.Daemon1"
def log(s): open(out, "a").write(s + "\n")

class Daemon(dbus.service.Object):
    def __init__(self, bus):
        super().__init__(bus, "/io/github/scott8420/NetScope/Daemon")
        self.shares = [["afp://tc01.local/Data/", "Data", "tc01.local", False],
                       ["smb://raspi.local/pub/", "pub", "raspi.local", True]]
        self.keyring = False
    def props(self):
        return {"Shares": dbus.Array([dbus.Struct((u, n, d, dbus.Boolean(m))) for u, n, d, m in self.shares],
                                     signature="(sssb)"),
                "AnyMounted": dbus.Boolean(any(s[3] for s in self.shares))}
    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="ss", out_signature="v")
    def Get(self, i, n): return self.props()[n]
    @dbus.service.method(dbus.PROPERTIES_IFACE, in_signature="s", out_signature="a{sv}")
    def GetAll(self, i): return self.props()
    @dbus.service.signal(dbus.PROPERTIES_IFACE, signature="sa{sv}as")
    def PropertiesChanged(self, i, ch, inv): pass
    def set(self, uri, on):
        for s in self.shares:
            if s[0] == uri: s[3] = on
        self.PropertiesChanged(IF, self.props(), dbus.Array([], signature="s"))
    @dbus.service.method(IF, in_signature="s", out_signature="s", async_callbacks=("ok", "err"))
    def Mount(self, uri, ok, err):
        log("Mount " + uri)
        if uri.startswith("afp://"):
            if not self.keyring: ok("password"); return
            def later():
                self.set(uri, True); log("Mounted " + uri); ok(""); return False
            GLib.timeout_add(3000, later); return
        self.set(uri, True); ok("")
    @dbus.service.method(IF)
    def TestKeyring(self): self.keyring = True; log("TestKeyring")
    @dbus.service.method(IF, in_signature="s", out_signature="s")
    def Unmount(self, uri):
        log("Unmount " + uri); self.set(uri, False); return ""
    @dbus.service.method(IF, out_signature="as")
    def UnmountAll(self):
        log("UnmountAll")
        for s in self.shares: s[3] = False
        self.PropertiesChanged(IF, self.props(), dbus.Array([], signature="s"))
        return dbus.Array([], signature="s")
    @dbus.service.method(IF)
    def Refresh(self): log("Refresh")

class App(dbus.service.Object):
    @dbus.service.method("org.freedesktop.Application", in_signature="sava{sv}")
    def ActivateAction(self, name, params, platform):
        log("ActivateAction " + name + " " + " ".join(str(p) for p in params))
    @dbus.service.method("org.freedesktop.Application", in_signature="a{sv}")
    def Activate(self, platform): log("Activate")

bus = dbus.SessionBus()
names = [dbus.service.BusName("io.github.scott8420.NetScope.Daemon", bus),
         dbus.service.BusName("io.github.scott8420.NetScope", bus)]
Daemon(bus); App(bus, "/io/github/scott8420/NetScope")
log("ready")
GLib.MainLoop().run()
