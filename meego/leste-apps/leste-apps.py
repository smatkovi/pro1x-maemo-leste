#!/usr/bin/python3
"""Leste applications in the MeeGo session.

The MeeGo boot runs Harmattan's user interface on Leste, and Leste's own
applications still work on the same X display: the camera (pro1x-camera on
libcamera), Firefox ESR, gPodder, the Hildon applications... This puts them
into Harmattan's application grid:

  leste-apps.py sync <harmattan tree>
      writes a Harmattan desktop entry (and an 80x80 icon) for every Leste
      application a menu would show (from /usr/share/applications or its
      hildon/ directory; not hidden, not a terminal program); entries of
      applications no longer installed are removed
  leste-apps.py serve [<harmattan tree>]
      the system bus service org.pro1x.LesteLauncher, whose Launch(s id)
      the entries call: it starts the application as Leste's user on the
      display, with Leste's session bus and PulseAudio. Given the tree, it
      also watches the menu entries and syncs again after packages added or
      removed applications.

/etc/pro1x-meego/leste-apps adjusts the choice, one Leste desktop id per
line: "-id" leaves an application out of the grid, "id" adds one that has
no menu entry of its own.

Runs as root on Leste (40-harmattan-session.sh). Only applications in the
grid can be started, by their desktop entry's Exec line.
"""
import configparser
import gettext
import glob
import os
import re
import shlex
import shutil
import subprocess
import sys

CONFIG = '/etc/pro1x-meego/leste-apps'
DESKTOP_DIRS = ('/usr/share/applications/hildon', '/usr/share/applications')
ICON_DIRS = ('/usr/share/icons', '/usr/share/pixmaps')
LOCALE_DIR = '/usr/share/locale'
ICON_SIZE = 80          # Harmattan's launcher icons
FALLBACK_ICON = 'general_applications'
PREFIX = 'pro1x-leste-'
NAME = 'org.pro1x.LesteLauncher'
PATH = '/org/pro1x/LesteLauncher'
USER = 'user'
SEARCH_PATH = '/usr/local/bin:/usr/bin:/bin:/usr/games:/usr/sbin:/sbin'


def adjustments():
    """(left out, added) desktop ids from the configuration file"""
    hide, add = set(), set()
    try:
        with open(CONFIG) as f:
            for line in f:
                line = line.strip()
                if line.startswith('-'):
                    hide.add(line[1:].strip())
                elif line and not line.startswith('#'):
                    add.add(line)
    except OSError:
        pass
    return hide, add


def read_entry(path):
    cfg = configparser.ConfigParser(interpolation=None, strict=False)
    cfg.optionxform = str
    try:
        cfg.read(path, encoding='utf-8')
    except (configparser.Error, UnicodeDecodeError):
        return None
    return cfg['Desktop Entry'] if cfg.has_section('Desktop Entry') else None


def shown(entry):
    """Whether a menu would show the entry (OnlyShowIn names other desktops)"""
    def true(key):
        return entry.get(key, '').strip().lower() == 'true'
    if entry.get('Type') != 'Application' or true('NoDisplay') or true('Hidden') or true('Terminal'):
        return False
    if entry.get('OnlyShowIn'):
        return False
    try_exec = entry.get('TryExec')
    return not try_exec or shutil.which(try_exec, path=SEARCH_PATH) is not None


def applications():
    """desktop id -> entry of the applications in the grid"""
    hide, add = adjustments()
    apps, seen = {}, set()
    for d in DESKTOP_DIRS:
        for path in sorted(glob.glob(os.path.join(d, '*.desktop'))):
            app = os.path.basename(path)[:-len('.desktop')]
            if app in seen:     # hildon/ comes first
                continue
            seen.add(app)
            entry = read_entry(path)
            if entry is not None and 'Exec' in entry and app not in hide and (app in add or shown(entry)):
                apps[app] = entry
    return apps


def names(entry):
    """Name and Name[locale] lines of the Harmattan entry"""
    name = entry.get('Name', '')
    domain = entry.get('X-Text-Domain')
    if not domain and re.fullmatch(r'[a-z0-9]+(_[a-z0-9]+)+', name):
        domain = 'maemo-af-desktop'     # hildon-desktop's default
    if not domain:
        return [(k, v) for k, v in entry.items() if k == 'Name' or k.startswith('Name[')]
    # Hildon applications name themselves with a message id of their gettext
    # domain (calc_ap_calculator)
    trans = {}
    for mo in glob.glob(os.path.join(LOCALE_DIR, '*', 'LC_MESSAGES', domain + '.mo')):
        try:
            with open(mo, 'rb') as f:
                text = gettext.GNUTranslations(f).gettext(name)
        except OSError:
            continue
        if text != name:
            trans[mo.split('/')[-3]] = text
    lines = [('Name', trans.get('en_GB') or trans.get('en_US') or name)]
    langs = set()
    # de_DE also stands for de
    for loc in sorted(trans, key=lambda l: (l.partition('_')[2].lower() != l.partition('_')[0], l)):
        lang = loc.partition('_')[0]
        if lang not in langs:
            langs.add(lang)
            lines.append(('Name[%s]' % lang, trans[loc]))
        if loc != lang:
            lines.append(('Name[%s]' % loc, trans[loc]))
    return lines


def icon_index():
    """icon name -> [(size, path)] over all icon themes and the pixmaps"""
    index = {}
    for top in ICON_DIRS:
        for root, dirs, files in os.walk(top):
            size = 0
            for part in root[len(top):].split('/'):
                m = re.fullmatch(r'(\d+)x\d+(@\d+x?)?', part)
                if m:
                    size = int(m.group(1))
            for f in files:
                name, ext = os.path.splitext(f)
                if ext in ('.png', '.svg', '.xpm'):
                    index.setdefault(name, []).append((size, os.path.join(root, f)))
    return index


def find_icon(name, index):
    """The picture for an Icon key: the smallest at least ICON_SIZE wide, else the largest"""
    from gi.repository import GdkPixbuf
    if not name:
        return None
    if os.path.isabs(name):
        if os.path.exists(name):
            return name
        name = os.path.basename(name)
    best = None
    for size, path in index.get(re.sub(r'\.(png|svg|xpm)$', '', name), []):
        if path.endswith('.svg'):
            size = 100000       # any size
        elif size == 0:
            info = GdkPixbuf.Pixbuf.get_file_info(path)
            size = info[1] if info and info[0] else 0
        key = (size < ICON_SIZE, size if size >= ICON_SIZE else -size)
        if best is None or key < best[0]:
            best = (key, path)
    return best and best[1]


def write_if_changed(path, data):
    """Leaves files that stay the same untouched (the launcher watches them)"""
    try:
        with open(path, 'rb') as f:
            if f.read() == data:
                return
    except OSError:
        pass
    with open(path + '.new', 'wb') as f:
        f.write(data)
    os.replace(path + '.new', path)


def sync(harmattan):
    import gi
    gi.require_version('GdkPixbuf', '2.0')
    from gi.repository import GdkPixbuf, GLib

    apps_dir = os.path.join(harmattan, 'usr/share/applications')
    icon_dir = os.path.join(harmattan, 'usr/share/pixmaps/pro1x-leste')
    os.makedirs(icon_dir, exist_ok=True)
    index = icon_index()
    apps = applications()
    for app, entry in apps.items():
        icon_path = ''
        src = find_icon(entry.get('Icon'), index) or find_icon(FALLBACK_ICON, index)
        if src:
            try:
                pixbuf = GdkPixbuf.Pixbuf.new_from_file_at_scale(src, ICON_SIZE, ICON_SIZE, True)
                ok, png = pixbuf.save_to_bufferv('png', [], [])
                icon_path = '/usr/share/pixmaps/pro1x-leste/%s.png' % app
                write_if_changed(os.path.join(harmattan, icon_path.lstrip('/')), png)
            except GLib.Error as e:
                print('leste-apps: icon of %s: %s' % (app, e.message), file=sys.stderr)
                icon_path = ''
        lines = ['[Desktop Entry]', 'Type=Application']
        lines += ['%s=%s' % kv for kv in names(entry)]
        if icon_path:
            lines.append('Icon=%s' % icon_path)
        lines.append('Exec=/usr/bin/dbus-send --system --type=method_call --dest=%s %s %s.Launch string:%s'
                     % (NAME, PATH, NAME, app))
        lines.append('X-Pro1X-Leste-App=%s' % app)
        write_if_changed(os.path.join(apps_dir, PREFIX + app + '.desktop'), ('\n'.join(lines) + '\n').encode())
    for path in glob.glob(os.path.join(apps_dir, PREFIX + '*.desktop')):
        app = os.path.basename(path)[len(PREFIX):-len('.desktop')]
        if app not in apps:
            os.remove(path)
    for path in glob.glob(os.path.join(icon_dir, '*.png')):
        if os.path.basename(path)[:-len('.png')] not in apps:
            os.remove(path)


def session_env():
    env = {'HOME': '/home/' + USER, 'USER': USER, 'LOGNAME': USER, 'DISPLAY': ':0',
           'XAUTHORITY': '/home/%s/.Xauthority' % USER, 'LANG': os.environ.get('LANG', 'en_US.UTF-8'),
           'PATH': '/usr/local/bin:/usr/bin:/bin:/usr/games',
           # Leste's Qt 5 programs crash on touch input after a blank with XI2
           'QT_XCB_NO_XI2': '1',
           # Firefox scrolls by touch only with XI2 touch events
           'MOZ_USE_XINPUT2': '1'}
    try:
        with open('/tmp/session_bus_address.user') as f:
            for line in f:
                if 'DBUS_SESSION_BUS_ADDRESS=' in line:
                    env['DBUS_SESSION_BUS_ADDRESS'] = line.split('=', 1)[1].strip().strip('\'"')
    except OSError:
        pass
    uid = int(subprocess.run(['id', '-u', USER], capture_output=True, text=True).stdout or 1000)
    env['XDG_RUNTIME_DIR'] = '/run/user/%d' % uid
    return env


def launch(app):
    entry = applications().get(app)
    if entry is None:
        return False
    args = [a for a in shlex.split(entry['Exec']) if not (a.startswith('%') and len(a) == 2)]
    runuser = shutil.which('runuser', path='/usr/sbin:/sbin:/usr/bin:/bin') or '/usr/sbin/runuser'
    subprocess.Popen([runuser, '-u', USER, '--'] + args, env=session_env(), cwd='/home/' + USER,
                     stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)
    return True


def serve(harmattan=None):
    import dbus
    import dbus.service
    from dbus.mainloop.glib import DBusGMainLoop
    from gi.repository import Gio, GLib

    class Launcher(dbus.service.Object):
        @dbus.service.method(NAME, in_signature='s', out_signature='b')
        def Launch(self, app):
            return launch(str(app))

    DBusGMainLoop(set_as_default=True)
    bus = dbus.SystemBus()
    name = dbus.service.BusName(NAME, bus, do_not_queue=True)
    Launcher(bus, PATH)

    # Packages add and remove menu entries (inotify, nothing polls): sync a
    # few seconds after the last change, when dpkg is done with them.
    monitors, pending = [], []

    def resync():
        pending.clear()
        try:
            sync(harmattan)
        except Exception as e:
            print('leste-apps: sync: %s' % e, file=sys.stderr)
        return False

    def changed(*args):
        if pending:
            GLib.source_remove(pending.pop())
        pending.append(GLib.timeout_add_seconds(3, resync))

    if harmattan:
        for path in DESKTOP_DIRS + (CONFIG,):
            monitor = Gio.File.new_for_path(path).monitor(Gio.FileMonitorFlags.NONE, None)
            monitor.connect('changed', changed)
            monitors.append(monitor)
    loop = GLib.MainLoop()
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, 15, loop.quit)
    loop.run()


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == 'sync':
        sync(sys.argv[2])
    elif len(sys.argv) in (2, 3) and sys.argv[1] == 'serve':
        serve(sys.argv[2] if len(sys.argv) == 3 else None)
    else:
        sys.exit('usage: leste-apps.py sync <harmattan tree> | serve [<harmattan tree>]')
