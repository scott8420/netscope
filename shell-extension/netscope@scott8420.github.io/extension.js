// NetScope -- the GNOME Quick Settings tile (milestone 11). GNOME Shell 46-50, ESM.
// Samson's tile (s011-s020), re-aimed at netscoped.
//
// The switch is lit while any pinned share is mounted. Switching it off
// unmounts them all; switching it on mounts the pinned shares. A share that
// needs a password goes to the NetScope app (its own dialog) -- a password is
// never typed into the Shell. Milestone 12: an AFP share whose password is in
// Keyring is mounted by netscoped itself; the row says "mounting…" meanwhile. Unmount / Unmount All go to netscoped.
//
//   netscoped  io.github.scott8420.NetScope.Daemon  /io/github/scott8420/NetScope/Daemon
//              (Shares, AnyMounted, Mount, Unmount, UnmountAll, Refresh -- daemon/netscoped.cpp)
//   the app    io.github.scott8420.NetScope  org.freedesktop.Application.ActivateAction
//              (mount(s), devices -- D-Bus activation starts it)

import GObject from 'gi://GObject';
import Gio from 'gi://Gio';
import GLib from 'gi://GLib';
import St from 'gi://St';
import Clutter from 'gi://Clutter';

import {Extension} from 'resource:///org/gnome/shell/extensions/extension.js';
import * as Main from 'resource:///org/gnome/shell/ui/main.js';
import * as PopupMenu from 'resource:///org/gnome/shell/ui/popupMenu.js';
import {QuickMenuToggle, SystemIndicator} from 'resource:///org/gnome/shell/ui/quickSettings.js';

const DAEMON = 'io.github.scott8420.NetScope.Daemon';
const DAEMON_PATH = '/io/github/scott8420/NetScope/Daemon';
const APP_ID = 'io.github.scott8420.NetScope';
const APP_PATH = '/io/github/scott8420/NetScope';

const DaemonXml = `
<node>
  <interface name="io.github.scott8420.NetScope.Daemon1">
    <property name="Shares"     type="a(sssb)" access="read"/>
    <property name="AnyMounted" type="b" access="read"/>
    <method name="Mount"><arg name="uri" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="Unmount"><arg name="uri" type="s" direction="in"/><arg name="result" type="s" direction="out"/></method>
    <method name="UnmountAll"><arg name="failed" type="as" direction="out"/></method>
    <method name="Refresh"/>
  </interface>
</node>`;
const DaemonProxy = Gio.DBusProxy.makeProxyWrapper(DaemonXml);

const MARK_OK = '✓ ';   // ✓
const MARK_NO = '✗ ';   // ✗

// "tc01.local" -> "tc01"
function shortHost(device) {
    return device.endsWith('.local') ? device.slice(0, -'.local'.length) : device;
}

// Ask the NetScope app to do something. D-Bus activation starts it when it is
// not running (install.sh puts in the .service file). The activation token
// lets the window come to the front on Wayland.
function appAction(name, param) {
    const platform = {};
    try {
        const info = Gio.DesktopAppInfo.new(`${APP_ID}.desktop`);
        const ctx = global.create_app_launch_context(0, -1);
        const token = info ? ctx.get_startup_notify_id(info, []) : null;
        if (token) {
            platform['activation-token'] = new GLib.Variant('s', token);
            platform['desktop-startup-id'] = new GLib.Variant('s', token);
        }
    } catch (e) {
        // no token: the window still opens, maybe behind
    }
    const args = param === undefined ? [] : [param];
    Gio.DBus.session.call(APP_ID, APP_PATH, 'org.freedesktop.Application', 'ActivateAction',
        new GLib.Variant('(sava{sv})', [name, args, platform]),
        null, Gio.DBusCallFlags.NONE, 20000, null,
        (conn, res) => {
            try {
                conn.call_finish(res);
            } catch (e) {
                console.warn(`NetScope tile: ${name}: ${e.message}`);
                try {
                    Gio.DesktopAppInfo.new(`${APP_ID}.desktop`)?.launch([], null);
                } catch (e2) {
                    Main.notifyError('NetScope', 'Could not start NetScope -- run install.sh');
                }
            }
        });
}

// One pinned share: name, where it lives / whether it's mounted, and the one
// button that fits. The row itself does the same.
const ShareItem = GObject.registerClass(
class ShareItem extends PopupMenu.PopupBaseMenuItem {
    _init(share, tile) {
        super._init({style_class: 'netscope-row'});
        const [uri, name, device, mounted] = share;
        const busy = tile.isBusy(uri);
        this._uri = uri;
        this._name = name;
        this._tile = tile;
        this._mounted = mounted;

        this.add_child(new St.Icon({
            icon_name: mounted ? 'folder-remote-symbolic' : 'network-server-symbolic',
            style_class: 'popup-menu-icon',
        }));
        const text = new St.BoxLayout({vertical: true, x_expand: true, y_align: Clutter.ActorAlign.CENTER});
        text.add_child(new St.Label({text: name, style_class: 'netscope-row-name'}));
        const kind = uri.startsWith('afp://') ? 'AFP' : uri.startsWith('smb://') ? 'SMB' :
                     uri.startsWith('sftp://') ? 'SFTP' : uri.startsWith('nfs://') ? 'NFS' : '';
        text.add_child(new St.Label({
            text: (busy ? 'mounting… · ' : mounted ? MARK_OK + 'mounted · ' : '') +
                `on ${shortHost(device)}` + (kind ? ` · ${kind}` : ''),
            style_class: 'netscope-row-sub',
        }));
        this.add_child(text);

        const btn = new St.Button({
            label: mounted ? 'Unmount' : 'Mount',
            style_class: 'button netscope-row-button',
            y_align: Clutter.ActorAlign.CENTER,
            reactive: !busy,
        });
        this._busy = busy;
        btn.connect('clicked', () => this._act());
        this.add_child(btn);
        this.connect('activate', () => this._act());
    }

    _act() {
        if (this._busy) return;
        if (this._mounted) this._tile.unmount(this._uri, this._name);
        else this._tile.mount(this._uri, this._name);
        this._tile.menu.close();
    }
});

const NetScopeToggle = GObject.registerClass(
class NetScopeToggle extends QuickMenuToggle {
    _init(ext) {
        this._gicon = Gio.icon_new_for_string(`${ext.path}/icons/netscope-logo-symbolic.svg`);
        super._init({title: 'NetScope', gicon: this._gicon, toggleMode: false});
        this._ext = ext;
        this._proxy = null;
        this._shares = [];
        this._busy = new Set();   // URIs being mounted (AFP takes ~15 s)

        this.menu.setHeader(this._gicon, 'NetScope', '');
        this._shareSection = new PopupMenu.PopupMenuSection();
        this.menu.addMenuItem(this._shareSection);
        this._statusItem = new PopupMenu.PopupMenuItem('', {activate: false, hover: false, can_focus: false});
        this._statusItem.label.add_style_class_name('netscope-status');
        this.menu.addMenuItem(this._statusItem);
        this.menu.addMenuItem(new PopupMenu.PopupSeparatorMenuItem());
        this._unmountAllItem = this.menu.addAction('Unmount All', () => this.unmountAll(), 'media-eject-symbolic');
        this.menu.addAction('Open NetScope', () => appAction('devices'), 'network-workgroup-symbolic');

        this.connect('clicked', () => this._onClicked());
        this.menu.connect('open-state-changed', (m, open) => {
            if (open) this._proxy?.RefreshRemote(() => {});
        });

        new DaemonProxy(Gio.DBus.session, DAEMON, DAEMON_PATH, (proxy, error) => {
            if (error) {
                console.warn(`NetScope tile: ${error.message}`);
                this._sync();
                return;
            }
            this._proxy = proxy;
            // An AFP mount from Keyring takes ~15 s cold, up to 90: don't time out at 25.
            proxy.set_default_timeout(120000);
            this._propsId = proxy.connect('g-properties-changed', () => this._sync());
            this._ownerId = proxy.connect('notify::g-name-owner', () => this._sync());
            this._sync();
        }, null, Gio.DBusProxyFlags.NONE);
        this._sync();
    }

    isBusy(uri) {
        return this._busy.has(uri);
    }

    _up() {
        return this._proxy !== null && this._proxy.g_name_owner !== null;
    }

    _sync() {
        const up = this._up();
        const shares = up ? (this._proxy.Shares ?? []) : [];
        const anyMounted = up && !!this._proxy.AnyMounted;
        this._shares = shares;

        this._shareSection.removeAll();
        for (const s of shares) this._shareSection.addMenuItem(new ShareItem(s, this));

        let subtitle;
        this._statusItem.visible = true;
        if (!up) {
            subtitle = 'Service off';
            this._statusItem.label.text = MARK_NO + 'netscoped is not running (install.sh starts it)';
        } else if (shares.length === 0) {
            subtitle = 'No pinned shares';
            this._statusItem.label.text = 'Pin a share in NetScope (Devices → Shares → Pin)';
        } else {
            const on = shares.filter(s => s[3]);
            const busy = shares.filter(s => this._busy.has(s[0]));
            subtitle = busy.length > 0 ? `Mounting ${busy[0][1]}…` : on.length === 1 ? `${on[0][1]} mounted` : on.length > 1 ? `${on.length} mounted` : 'Nothing mounted';
            this._statusItem.visible = false;
        }
        this.subtitle = subtitle;
        this.menu.setHeader(this._gicon, 'NetScope', subtitle);
        this.checked = anyMounted;
        this._unmountAllItem.setSensitive(anyMounted);
        this._ext.indicatorVisible(anyMounted);
    }

    _onClicked() {
        if (this.checked) {
            this.unmountAll();
            return;
        }
        const todo = this._shares.filter(s => !s[3]);
        if (todo.length === 0) {
            appAction('devices');   // nothing pinned: the Devices page, to pin something
            return;
        }
        this._mountEach(todo);
    }

    // One after another; the first that needs a password goes to the app and
    // the rest wait for the next click (one dialog at a time).
    _mountEach(list) {
        if (list.length === 0) return;
        const [uri, name] = list[0];
        this._mount(uri, name, needed => {
            if (!needed) this._mountEach(list.slice(1));
        });
    }

    _mount(uri, name, then) {
        if (!this._proxy || this._busy.has(uri)) return;
        this._busy.add(uri);
        this._sync();
        this._proxy.MountRemote(uri, (result, error) => {
            this._busy.delete(uri);
            this._sync();
            if (error) {
                Main.notifyError('NetScope', `Couldn't mount ${name}: ${error.message}`);
                then?.(false);
                return;
            }
            const [r] = result;
            if (r === 'password') {
                appAction('mount', new GLib.Variant('s', uri));
                then?.(true);
                return;
            }
            if (r !== '') Main.notifyError('NetScope', `Couldn't mount ${name}: ${r}`);
            then?.(false);
        });
    }

    mount(uri, name) {
        this._mount(uri, name, null);
    }

    unmount(uri, name) {
        this._proxy?.UnmountRemote(uri, (result, error) => {
            const r = error ? error.message : result[0];
            if (r !== '') Main.notifyError('NetScope', `Couldn't unmount ${name}: ${r}`);
        });
    }

    unmountAll() {
        this._proxy?.UnmountAllRemote((result, error) => {
            const failed = error ? [error.message] : result[0];
            if (failed.length > 0) Main.notifyError('NetScope', `Still mounted: ${failed.join(', ')}`);
        });
    }

    destroy() {
        if (this._proxy) {
            if (this._propsId) this._proxy.disconnect(this._propsId);
            if (this._ownerId) this._proxy.disconnect(this._ownerId);
            this._proxy = null;
        }
        super.destroy();
    }
});

const NetScopeIndicator = GObject.registerClass(
class NetScopeIndicator extends SystemIndicator {
    _init(ext) {
        super._init();
        // In the top bar only while something pinned is mounted.
        this._icon = this._addIndicator();
        this._icon.gicon = Gio.icon_new_for_string(`${ext.path}/icons/netscope-logo-symbolic.svg`);
        this._icon.visible = false;
    }
});

export default class NetScopeExtension extends Extension {
    enable() {
        this._indicator = new NetScopeIndicator(this);
        this._toggle = new NetScopeToggle(this);
        this._indicator.quickSettingsItems.push(this._toggle);
        Main.panel.statusArea.quickSettings.addExternalIndicator(this._indicator);
    }

    indicatorVisible(v) {
        if (this._indicator) this._indicator._icon.visible = v;
    }

    disable() {
        this._indicator?.quickSettingsItems.forEach(i => i.destroy());
        this._indicator?.destroy();
        this._indicator = null;
        this._toggle = null;
    }
}
